#include "casProxyClient.h"
#include <array>
#include <future>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

// Real loopback TCP with immediate replies, duplicate replies and forced EOF.
class Server {
    struct Peer : std::enable_shared_from_this<Peer> {
        asio::ip::tcp::socket socket;
        Server& server;
        std::array<uint8_t, 4> header{};
        std::vector<uint8_t> request;
        explicit Peer(asio::io_context& io, Server& server) : socket(io), server(server) {}
        void read() {
            auto self = shared_from_this();
            asio::async_read(socket, asio::buffer(header), [self](std::error_code ec, size_t) {
                if (ec) return;
                uint32_t length;
                memcpy(&length, self->header.data(), 4);
                length = casproxy::swapEndian32(length);
                if (length > 1024) return;
                self->request.resize(length);
                asio::async_read(self->socket, asio::buffer(self->request), [self](std::error_code error, size_t) {
                    if (!error) self->reply();
                });
            });
        }
        void reply() {
            if (server.drop.load()) {
                asio::error_code ec;
                socket.close(ec);
                return;
            }
            if (server.stall.load()) return;
            casproxy::StreamReader reader(request);
            uint32_t id;
            require(reader.readBe(id), "missing request ID");
            casproxy::SCardEstablishContextResponse response;
            response.packetId = id;
            response.resultCode = 0;
            response.apiReturn = SCARD_S_SUCCESS;
            response.hContext = 1234;
            casproxy::StreamWriter payload;
            if (server.wrongOpcode.load()) {
                casproxy::SCardReleaseContextResponse wrong;
                wrong.packetId = id;
                wrong.resultCode = 0;
                wrong.apiReturn = SCARD_S_SUCCESS;
                wrong.pack(payload);
            } else {
                response.pack(payload);
            }
            casproxy::StreamWriter frame;
            frame.writeBe(static_cast<uint32_t>(payload.buffer.size()));
            frame.buffer.insert(frame.buffer.end(), payload.buffer.begin(), payload.buffer.end());
            auto data = std::make_shared<std::vector<uint8_t>>(std::move(frame.buffer));
            if (server.duplicate.load()) {
                const auto copy = *data;
                data->insert(data->end(), copy.begin(), copy.end());
            }
            auto self = shared_from_this();
            asio::async_write(socket, asio::buffer(*data), [self, data](std::error_code ec, size_t) {
                if (!ec) self->read();
            });
        }
    };
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor;
    std::thread worker;
    std::vector<std::shared_ptr<Peer>> peers;
    void accept() {
        auto peer = std::make_shared<Peer>(io, *this);
        acceptor.async_accept(peer->socket, [this, peer](std::error_code ec) {
            if (!ec) {
                peers.push_back(peer);
                peer->read();
                accept();
            }
        });
    }
public:
    std::atomic<bool> drop{false};
    std::atomic<bool> duplicate{false};
    std::atomic<bool> stall{false};
    std::atomic<bool> wrongOpcode{false};
    explicit Server(uint16_t port)
        : acceptor(io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port)) {
        accept();
        worker = std::thread([this] { io.run(); });
    }
    ~Server() {
        io.stop();
        worker.join();
    }
};

static bool request(CasProxyClient& client) {
    SCARDCONTEXT context = 0;
    return client.scardEstablishContext(SCARD_SCOPE_USER, nullptr, nullptr, &context) == SCARD_S_SUCCESS && context == 1234;
}

int main() {
    try {
        // Keep a port bound but not listening to deterministically refuse TCP.
        asio::io_context io;
        asio::ip::tcp::acceptor reservation(io);
        reservation.open(asio::ip::tcp::v4());
        reservation.bind({asio::ip::address_v4::loopback(), 0});
        const auto port = reservation.local_endpoint().port();
        CasProxyClient client("127.0.0.1", port);
        for (int i = 0; i < 30; ++i) require(!request(client), "refused connection must fail");
        std::fprintf(stderr, "refusal passed\n");
        reservation.close();
        Server server(port);
        require(request(client), "client must recover after initial connection failure");
        server.wrongOpcode = true;
        require(!request(client), "wrong response opcode must fail without crashing");
        server.wrongOpcode = false;
        require(request(client), "wrong response opcode must allow reconnect");
        for (int i = 0; i < 30; ++i) {
            client.close();
            client.connect();
            require(request(client), "fast connect/close must preserve future lifetime");
        }
        std::fprintf(stderr, "fast connections passed\n");
        server.duplicate = true;
        std::vector<std::future<void>> callers;
        for (int i = 0; i < 8; ++i) {
            callers.push_back(std::async(std::launch::async, [&] {
                for (int j = 0; j < 30; ++j) require(request(client), "concurrent caller failed");
            }));
        }
        for (auto& caller : callers) caller.get();
        std::fprintf(stderr, "concurrent requests passed\n");
        server.duplicate = false;
        server.drop = true;
        require(!request(client), "EOF must fail the pending request");
        server.drop = false;
        require(request(client), "EOF must allow reconnect");
        std::fprintf(stderr, "starting stall check\n");
        server.stall = true;
        auto pending = std::async(std::launch::async, [&] { return request(client); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        client.close();
        require(!pending.get(), "explicit close must release a pending request");
        server.stall = false;
        require(request(client), "explicit close must allow reconnect");
        std::fprintf(stderr, "starting stall check\n");
        server.stall = true;
        const auto start = std::chrono::steady_clock::now();
        bool timedOut = false;
        try { request(client); } catch (const std::runtime_error&) { timedOut = true; }
        require(timedOut && std::chrono::steady_clock::now() - start < std::chrono::seconds(12),
                "unresponsive server must time out");
        server.stall = false;
        require(request(client), "request timeout must allow reconnect");
        std::puts("PASS: refusal, wrong opcode, fast completion, concurrent requests, duplicates, EOF, close and timeout recovery");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
