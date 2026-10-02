#include "casProxyClient.h"
#include <array>
#include <future>

CasProxyClient::CasProxyClient(const std::string& host, uint16_t port)
    : host(host), port(port), resolver(io_context), connectionTimer(io_context), workGuard(asio::make_work_guard(io_context)) {
    thread = std::thread([this]() { io_context.run(); });
}

CasProxyClient::~CasProxyClient() {
    close();
    workGuard.reset();
    io_context.stop();
    if (thread.joinable()) thread.join();
}

void CasProxyClient::connect() {
    std::lock_guard<std::mutex> lock(mutex);
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future(); // Acquire before posting any completion.
    asio::post(io_context, [this, promise]() {
        ensureConnected([promise](bool ok) { promise->set_value(ok); });
    });
    if (future.wait_for(std::chrono::milliseconds(kConnectionTimeoutMs + 1000)) != std::future_status::ready || !future.get()) {
        asio::post(io_context, [this]() { disconnect(); });
        throw std::runtime_error("Failed to connect to CasProxyServer");
    }
}

void CasProxyClient::ensureConnected(std::function<void(bool)> completion) {
    if (connected) {
        completion(true);
        return;
    }
    connectionWaiters.push_back(std::move(completion));
    if (connecting) return;
    connecting = true;
    const auto generation = ++connectionGeneration;
    socket = std::make_shared<asio::ip::tcp::socket>(io_context);
    const auto currentSocket = socket;
    connectionTimer.expires_after(std::chrono::milliseconds(kConnectionTimeoutMs));
    connectionTimer.async_wait([this, generation](const std::error_code& ec) {
        if (!ec && generation == connectionGeneration && !connected) onFail();
    });
    resolver.async_resolve(host, std::to_string(port),
        [this, generation, currentSocket](std::error_code ec, asio::ip::tcp::resolver::results_type results) {
            if (generation != connectionGeneration) return;
            if (ec) {
                onFail();
                return;
            }
            asio::async_connect(*currentSocket, results,
                [this, generation, currentSocket](std::error_code error, const asio::ip::tcp::endpoint&) {
                    if (generation != connectionGeneration) return;
                    if (error) {
                        onFail();
                    } else {
                        onOpen();
                        doRead();
                    }
                });
        });
}

void CasProxyClient::close() {
    if (io_context.get_executor().running_in_this_thread()) {
        disconnect();
        return;
    }
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    asio::post(io_context, [this, promise]() {
        disconnect();
        promise->set_value();
    });
    future.get();
}

void CasProxyClient::disconnect() {
    // Keep the executor alive for reconnects. Only destruction stops it.
    ++connectionGeneration;
    connected = false;
    connecting = false;
    resolver.cancel();
    connectionTimer.cancel();
    if (socket) {
        asio::error_code ec;
        socket->cancel(ec);
        socket->close(ec);
        socket.reset();
    }
    // In-flight writes retain their own shared buffer until cancellation completes.
    sendQueue.clear();
    for (auto& [id, promise] : mapResponsePromise) promise->set_value(nullptr);
    mapResponsePromise.clear();
    auto waiters = std::move(connectionWaiters);
    connectionWaiters.clear();
    for (auto& completion : waiters) completion(false);
}

void CasProxyClient::onOpen() {
    connectionTimer.cancel();
    connected = true;
    connecting = false;
    auto waiters = std::move(connectionWaiters);
    connectionWaiters.clear();
    for (auto& completion : waiters) completion(true);
}

void CasProxyClient::onFail() {
    disconnect();
}

void CasProxyClient::doRead() {
    const auto currentSocket = socket;
    const auto generation = connectionGeneration;
    auto lengthBytes = std::make_shared<std::array<uint8_t, 4>>();
    asio::async_read(*currentSocket, asio::buffer(*lengthBytes),
        [this, generation, currentSocket, lengthBytes](std::error_code ec, std::size_t) {
            if (generation != connectionGeneration) return;
            if (ec) {
                disconnect(); // EOF is also a disconnect, not an idle connection.
                return;
            }
            uint32_t length = 0;
            memcpy(&length, lengthBytes->data(), sizeof(length));
            length = casproxy::swapEndian32(length);
            if (length < 12 || length > 100 * 1024) {
                disconnect();
                return;
            }
            readPacketData(length);
        });
}

void CasProxyClient::readPacketData(uint32_t length) {
    const auto currentSocket = socket;
    const auto generation = connectionGeneration;
    auto data = std::make_shared<std::vector<uint8_t>>(length);
    asio::async_read(*currentSocket, asio::buffer(*data),
        [this, generation, currentSocket, data](std::error_code ec, std::size_t) {
            if (generation != connectionGeneration) return;
            if (ec) {
                disconnect();
                return;
            }
            handleResponse(*data);
            doRead();
        });
}

void CasProxyClient::handleResponse(const std::vector<uint8_t>& data) {
    casproxy::StreamReader reader(data);
    uint32_t id, resultCode, opcodeValue;
    if (!reader.read(id) || !reader.read(resultCode) || !reader.read(opcodeValue)) return;
    id = casproxy::swapEndian32(id);
    resultCode = casproxy::swapEndian32(resultCode);
    auto res = casproxy::ResponseFactory::create(static_cast<casproxy::Opcode>(casproxy::swapEndian32(opcodeValue)));
    if (!res || !res->unpack(id, resultCode, reader)) return;
    auto it = mapResponsePromise.find(id);
    if (it != mapResponsePromise.end()) {
        it->second->set_value(std::move(res));
        mapResponsePromise.erase(it); // A duplicate response cannot fulfill twice.
    }
}

std::shared_ptr<casproxy::ResponseBase> CasProxyClient::sendRequest(casproxy::RequestBase& req) {
    std::lock_guard<std::mutex> lock(mutex);
    casproxy::StreamWriter writer;
    req.packetId = packetId.fetch_add(1, std::memory_order_relaxed);
    req.pack(writer);
    const auto id = req.packetId;
    auto packet = std::make_shared<std::vector<uint8_t>>(writer.buffer.size() + 4);
    const uint32_t length = casproxy::swapEndian32(static_cast<uint32_t>(writer.buffer.size()));
    memcpy(packet->data(), &length, 4);
    memcpy(packet->data() + 4, writer.buffer.data(), writer.buffer.size());
    auto promise = std::make_shared<std::promise<std::shared_ptr<casproxy::ResponseBase>>>();
    auto future = promise->get_future();
    asio::post(io_context, [this, id, packet, promise]() {
        ensureConnected([this, id, packet, promise](bool ok) {
            if (!ok) {
                promise->set_value(nullptr);
                return;
            }
            mapResponsePromise.emplace(id, promise);
            sendQueue.push_back(packet);
            if (sendQueue.size() == 1) doWrite();
        });
    });
    // Bound the entire request, including resolving and connecting.
    if (future.wait_for(std::chrono::milliseconds(kConnectionTimeoutMs + kRequestTimeoutMs)) != std::future_status::ready) {
        asio::post(io_context, [this]() { disconnect(); });
        throw std::runtime_error("Timeout while requesting CasProxyServer");
    }
    return future.get();
}

void CasProxyClient::doWrite() {
    if (sendQueue.empty()) return;
    const auto packet = sendQueue.front();
    const auto currentSocket = socket;
    const auto generation = connectionGeneration;
    asio::async_write(*currentSocket, asio::buffer(*packet),
        [this, generation, currentSocket, packet](std::error_code ec, std::size_t) {
            if (generation != connectionGeneration) return;
            if (ec) {
                disconnect();
            } else {
                sendQueue.pop_front();
                doWrite();
            }
        });
}

LONG CasProxyClient::scardEstablishContext(DWORD dwScope, LPCVOID pvReserved1, LPCVOID pvReserved2, LPSCARDCONTEXT phContext) {
    casproxy::SCardEstablishContextRequest req;
    req.dwScope = dwScope;

    auto r = sendRequest(req);
    if (!r) {
        return SCARD_F_INTERNAL_ERROR;
    }

    auto res = std::dynamic_pointer_cast<casproxy::SCardEstablishContextResponse>(r);
    if (res->resultCode != 0) {
        throw std::runtime_error(std::string("CasProxyServer returned an error response"));
    }

    if (phContext) {
        *phContext = res->hContext;
    }

    return res->apiReturn;
}

LONG CasProxyClient::scardReleaseContext(SCARDCONTEXT hContext) {
    casproxy::SCardReleaseContextRequest req;
    req.hContext = hContext;

    auto r = sendRequest(req);
    if (!r) {
        return SCARD_F_INTERNAL_ERROR;
    }

    auto res = std::dynamic_pointer_cast<casproxy::SCardReleaseContextResponse>(r);
    if (res->resultCode != 0) {
        throw std::runtime_error(std::string("CasProxyServer returned an error response"));
    }

    return res->apiReturn;
}

LONG CasProxyClient::scardListReaders(SCARDCONTEXT hContext, LPCSTR mszGroups, LPSTR mszReaders, LPDWORD pcchReaders) {
    if (pcchReaders == nullptr) {
        return SCARD_E_INVALID_PARAMETER;
    }

    if (*pcchReaders == SCARD_AUTOALLOCATE) {
        uint32_t readersLength = 0;
        {
            casproxy::SCardListReadersRequest req;
            req.hContext = hContext;
            if (mszGroups != nullptr) {
                req.groups = mszGroups;
            }

            req.readersLength = 0;
            auto r = sendRequest(req);
            if (!r) {
                return SCARD_F_INTERNAL_ERROR;
            }

            auto res = std::dynamic_pointer_cast<casproxy::SCardListReadersResponse>(r);
            if (res->resultCode != 0) {
                throw std::runtime_error(std::string("CasProxyServer returned an error response"));
            }

            if (res->apiReturn != SCARD_S_SUCCESS) {
                return res->apiReturn;
            }

            if (res->readersLength == 1) {
                *(LPSTR*)mszReaders = new char[1];
                (*(LPSTR*)mszReaders)[0] = 0;
                return res->apiReturn;
            }

            readersLength = res->readersLength;
        }

        {
            casproxy::SCardListReadersRequest req;
            req.hContext = hContext;
            if (mszGroups != nullptr) {
                req.groups = mszGroups;
            }
            req.readersLength = readersLength;

            auto r = sendRequest(req);
            if (!r) {
                return SCARD_F_INTERNAL_ERROR;
            }

            auto res = std::dynamic_pointer_cast<casproxy::SCardListReadersResponse>(r);
            if (res->resultCode != 0) {
                throw std::runtime_error(std::string("CasProxyServer returned an error response"));
            }

            if (res->apiReturn != SCARD_S_SUCCESS) {
                return res->apiReturn;
            }

            if (readersLength == 0) {
                return res->apiReturn;
            }

            *(LPSTR*)mszReaders = new char[res->readers.size()];
            memcpy(*(LPSTR*)mszReaders, res->readers.data(), res->readers.size());

            return res->apiReturn;
        }
    }
    else {
        casproxy::SCardListReadersRequest req;
        req.hContext = hContext;
        if (mszGroups != nullptr) {
            req.groups = mszGroups;
        }
        req.readersLength = *pcchReaders;

        auto r = sendRequest(req);
        if (!r) {
            return SCARD_F_INTERNAL_ERROR;
        }

        auto res = std::dynamic_pointer_cast<casproxy::SCardListReadersResponse>(r);
        if (res->resultCode != 0) {
            throw std::runtime_error(std::string("CasProxyServer returned an error response"));
        }

        if (mszReaders != nullptr) {
            memcpy(mszReaders, res->readers.data(), std::min(*pcchReaders, static_cast<DWORD>(res->readers.size())));
        }
        *pcchReaders = res->readersLength;

        return res->apiReturn;
    }

}

LONG CasProxyClient::scardConnect(SCARDCONTEXT hContext, LPCSTR szReader, DWORD dwShareMode, DWORD dwPreferredProtocols, LPSCARDHANDLE phCard, LPDWORD pdwActiveProtocol) {
    casproxy::SCardConnectRequest req;
    req.hContext = hContext;
    req.szReader = szReader;
    req.dwShareMode = dwShareMode;
    req.dwPreferredProtocols = dwPreferredProtocols;

    auto r = sendRequest(req);
    if (!r) {
        return SCARD_F_INTERNAL_ERROR;
    }

    auto res = std::dynamic_pointer_cast<casproxy::SCardConnectResponse>(r);
    if (res->resultCode != 0) {
        throw std::runtime_error(std::string("CasProxyServer returned an error response"));
    }

    if (phCard != nullptr) {
        *phCard = res->hCard;
    }
    if (pdwActiveProtocol != nullptr) {
        *pdwActiveProtocol = res->dwActiveProtocol;
    }

    return res->apiReturn;
}

LONG CasProxyClient::scardReconnect(SCARDHANDLE hCard, DWORD dwShareMode, DWORD dwPreferredProtocols, DWORD dwInitialization, LPDWORD pdwActiveProtocol) {
    casproxy::SCardReconnectRequest req;
    req.hCard = hCard;
    req.dwShareMode = dwShareMode;
    req.dwPreferredProtocols = dwPreferredProtocols;
    req.dwInitialization = dwInitialization;

    auto r = sendRequest(req);
    if (!r) {
        return SCARD_F_INTERNAL_ERROR;
    }

    auto res = std::dynamic_pointer_cast<casproxy::SCardReconnectResponse>(r);
    if (res->resultCode != 0) {
        throw std::runtime_error(std::string("CasProxyServer returned an error response"));
    }

    if (pdwActiveProtocol != nullptr) {
        *pdwActiveProtocol = res->dwActiveProtocol;
    }

    return res->apiReturn;
}

LONG CasProxyClient::scardDisconnect(SCARDHANDLE hCard, DWORD dwDisposition) {
    casproxy::SCardDisconnectRequest req;
    req.hCard = hCard;
    req.dwDisposition = dwDisposition;

    auto r = sendRequest(req);
    if (!r) {
        return SCARD_F_INTERNAL_ERROR;
    }

    auto res = std::dynamic_pointer_cast<casproxy::SCardDisconnectResponse>(r);
    if (res->resultCode != 0) {
        throw std::runtime_error(std::string("CasProxyServer returned an error response"));
    }

    return res->apiReturn;
}

LONG CasProxyClient::scardBeginTransaction(SCARDHANDLE hCard) {
    casproxy::SCardBeginTransactionRequest req;
    req.hCard = hCard;

    auto r = sendRequest(req);
    if (!r) {
        return SCARD_F_INTERNAL_ERROR;
    }

    auto res = std::dynamic_pointer_cast<casproxy::SCardBeginTransactionResponse>(r);
    if (res->resultCode != 0) {
        throw std::runtime_error(std::string("CasProxyServer returned an error response"));
    }

    return res->apiReturn;
}

LONG CasProxyClient::scardEndTransaction(SCARDHANDLE hCard, DWORD dwDisposition) {
    casproxy::SCardEndTransactionRequest req;
    req.hCard = hCard;
    req.dwDisposition = dwDisposition;

    auto r = sendRequest(req);
    if (!r) {
        return SCARD_F_INTERNAL_ERROR;
    }

    auto res = std::dynamic_pointer_cast<casproxy::SCardEndTransactionResponse>(r);
    if (res->resultCode != 0) {
        throw std::runtime_error(std::string("CasProxyServer returned an error response"));
    }

    return res->apiReturn;
}

LONG CasProxyClient::scardTransmit(SCARDHANDLE hCard, LPCSCARD_IO_REQUEST pioSendPci, LPCBYTE pbSendBuffer, DWORD cbSendLength, LPSCARD_IO_REQUEST pioRecvPci, LPBYTE pbRecvBuffer, LPDWORD pcbRecvLength) {
    casproxy::SCardTransmitRequest req;
    req.hCard = hCard;
    if (pioSendPci == nullptr) {
        req.sendPci = 3;
    }
    else {
        if (pioSendPci == SCARD_PCI_T1) {
            req.sendPci = 1;
        }
        else {
            return SCARD_F_INTERNAL_ERROR;
        }
    }

    req.sendBuffer = std::vector<uint8_t>((uint8_t*)pbSendBuffer, (uint8_t*)pbSendBuffer + cbSendLength);
    if (pioRecvPci == nullptr) {
        req.isRecvPciNull = true;
    }
    else {
        req.recvPciProtocol = pioRecvPci->dwProtocol;
        req.recvPciLength = pioRecvPci->cbPciLength;
    }

    req.recvLength = *pcbRecvLength;

    auto r = sendRequest(req);
    if (!r) {
        return SCARD_F_INTERNAL_ERROR;
    }

    auto res = std::dynamic_pointer_cast<casproxy::SCardTransmitResponse>(r);
    if (res->resultCode != 0) {
        throw std::runtime_error(std::string("CasProxyServer returned an error response"));
    }

    if (pbRecvBuffer != nullptr) {
        memcpy(pbRecvBuffer, res->recvBuffer.data(), std::min(*pcbRecvLength, static_cast<DWORD>(res->recvBuffer.size())));
    }

    *pcbRecvLength = res->recvLength;

    if (pioRecvPci != nullptr) {
        if (!res->isRecvPciNull) {
            pioRecvPci->dwProtocol = res->recvPciProtocol;
            pioRecvPci->cbPciLength = res->recvPciLength;
        }
    }

    return res->apiReturn;
}

LONG CasProxyClient::scardGetAttrib(SCARDHANDLE hCard, DWORD dwAttrId, LPBYTE pbAttr, LPDWORD pcbAttrLen) {
    casproxy::SCardGetAttribRequest req;
    req.hCard = hCard;
    req.dwAttrId = dwAttrId;
    req.attrLength = *pcbAttrLen;

    auto r = sendRequest(req);
    if (!r) {
        return SCARD_F_INTERNAL_ERROR;
    }

    auto res = std::dynamic_pointer_cast<casproxy::SCardGetAttribResponse>(r);
    if (res->resultCode != 0) {
        throw std::runtime_error(std::string("CasProxyServer returned an error response"));
    }

    if (pbAttr != nullptr) {
        memcpy(pbAttr, res->attrBuffer.data(), std::min(*pcbAttrLen, static_cast<DWORD>(res->attrBuffer.size())));
    }

    *pcbAttrLen = res->attrLength;

    return res->apiReturn;
}

LONG CasProxyClient::scardFreeMemory(SCARDCONTEXT hContext, LPCVOID pvMem) {
    delete[](char*)pvMem;
    return SCARD_S_SUCCESS;
}
