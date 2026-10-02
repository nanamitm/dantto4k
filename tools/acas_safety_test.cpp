// Uses a fake card to exercise real worker scheduling and key publication.
#include "casProxyClient.h"
#include "acasHandler.h"
#include "mmtp.h"
#include <condition_variable>
#include <future>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

class FakeCard : public ISmartCard {
public:
    bool fail = false;
    bool block = false;
    bool entered = false;
    std::mutex mutex;
    std::condition_variable cv;
    bool init() override { return true; }
    void connect() override {}
    uint32_t reconnect() override { return SCARD_S_SUCCESS; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    bool isInited() const override { return true; }
    std::vector<std::string> getReaders() const override { return {}; }
    void setSmartCardReaderName(const std::string&) override {}
    std::string getSmartCardReaderName() const override { return {}; }
    uint32_t transmit(const std::vector<uint8_t>& message, ApduResponse& response) override {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return !block; });
        if (fail) return SCARD_F_INTERNAL_ERROR;
        if (message[1] == 0xA0) {
            std::vector<uint8_t> nonce(message.begin() + 13, message.begin() + 21);
            std::vector<uint8_t> plain(std::begin(AcasCard::masterKey), std::end(AcasCard::masterKey));
            plain.insert(plain.end(), nonce.begin(), nonce.end());
            plain.insert(plain.end(), 8, 0x42);
            auto kcl = SHA256::hash(plain);
            plain.assign(kcl.begin(), kcl.end());
            plain.insert(plain.end(), nonce.begin(), nonce.end());
            auto hash = SHA256::hash(plain);
            std::vector<uint8_t> bytes(6, 0);
            bytes.insert(bytes.end(), 8, 0x42);
            bytes.insert(bytes.end(), hash.begin(), hash.end());
            response = ApduResponse(0x90, 0, bytes);
        } else {
            // Any 32-byte response produces a valid pair of keys after XOR.
            response = ApduResponse(0x90, 0, std::vector<uint8_t>(38, 0x24));
        }
        return SCARD_S_SUCCESS;
    }
protected:
    void beginTransaction() override {}
    void endTransaction() override {}
};

static MmtTlv::Mmtp packet() {
    MmtTlv::Mmtp result;
    MmtTlv::ExtensionHeaderScrambling scrambling;
    scrambling.encryptionFlag = MmtTlv::EncryptionFlag::EVEN;
    result.extensionHeaderScrambling = scrambling;
    result.packetId = 0x100;
    result.packetSequenceNumber = 1;
    result.payload.assign(32, 0x5a);
    return result;
}

int main() {
    try {
        AcasHandler handler;
        auto card = std::make_unique<FakeCard>();
        auto* fake = card.get();
        handler.setSmartCard(std::move(card));
        std::vector<uint8_t> ecm(32, 0x11);
        handler.onEcm(ecm);
        auto good = packet();
        require(handler.decrypt(good), "successful ECM must publish keys");
        {
            std::lock_guard<std::mutex> lock(fake->mutex);
            fake->fail = true;
        }
        ecm[0]++;
        handler.onEcm(ecm);
        auto bad = packet();
        const auto original = bad.payload;
        require(!handler.decrypt(bad) && bad.payload == original,
                "failed ECM must not decrypt with the previous key");
        {
            std::lock_guard<std::mutex> lock(fake->mutex);
            fake->fail = false;
        }
        handler.onEcm(ecm);
        require(handler.decrypt(bad), "identical ECM must retry after failure");

        {
            std::lock_guard<std::mutex> lock(fake->mutex);
            fake->block = true;
            fake->entered = false;
        }
        ecm[0]++;
        handler.onEcm(ecm);
        {
            std::unique_lock<std::mutex> lock(fake->mutex);
            require(fake->cv.wait_for(lock, std::chrono::seconds(2), [&] { return fake->entered; }),
                    "worker did not enter card transaction");
        }
        // Queue a second task behind the blocked transaction, then cancel both.
        ecm[0]++;
        handler.onEcm(ecm);
        const auto start = std::chrono::steady_clock::now();
        handler.clear();
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
                "clear must not wait for card I/O or a canceled queue");
        auto afterClear = packet();
        require(!handler.decrypt(afterClear), "clear must invalidate all old keys");
        {
            std::lock_guard<std::mutex> lock(fake->mutex);
            fake->block = false;
        }
        fake->cv.notify_all();
        handler.onEcm(ecm);
        require(handler.decrypt(afterClear), "new generation must recover after cancellation");
        for (int i = 0; i < 500; ++i) {
            handler.onEcm(ecm);
            handler.clear();
        }
        handler.onEcm(ecm);
        require(handler.decrypt(afterClear), "rapid enqueue/cancel must leave worker usable");
        std::puts("PASS: ECM success, failure, identical retry, cancellation and generation recovery");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
