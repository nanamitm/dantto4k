// Uses a fake card to exercise real worker scheduling and key publication.
#include "casProxyClient.h"
#include "acasHandler.h"
#include "mmtp.h"
#include "config.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
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
    int transmits = 0;
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
        ++transmits;
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

// Releases a blocked fake card on scope exit, so a failing check cannot leave
// the worker stuck in transmit() and hang the handler destructor.
struct Unblock {
    FakeCard& card;
    ~Unblock() {
        {
            std::lock_guard<std::mutex> lock(card.mutex);
            card.block = false;
        }
        card.cv.notify_all();
    }
};

static MmtTlv::Mmtp packet(MmtTlv::EncryptionFlag flag = MmtTlv::EncryptionFlag::EVEN) {
    MmtTlv::Mmtp result;
    MmtTlv::ExtensionHeaderScrambling scrambling;
    scrambling.encryptionFlag = flag;
    result.extensionHeaderScrambling = scrambling;
    result.packetId = 0x100;
    result.packetSequenceNumber = 1;
    result.payload.assign(32, 0x5a);
    return result;
}

class FakeEmmCard : public FakeCard {
public:
    std::array<uint8_t, 6> id{1, 2, 3, 4, 5, 6};
    int initializations = 0;
    int updates = 0;
    int reconnects = 0;
    int resets = 0;
    bool shortInit = false;
    bool reject = false;
    bool blockEmm = false;
    bool emmEntered = false;
    bool transaction = false;
    bool replaceOnReconnect = false;
    std::vector<uint8_t> lastMessage;
    uint32_t reconnect() override {
        std::lock_guard lock(mutex);
        ++reconnects;
        if (replaceOnReconnect) id[0] = 0x42;
        return SCARD_S_SUCCESS;
    }
    uint32_t transmit(const std::vector<uint8_t>& message, ApduResponse& response) override {
        require(transaction, "EMM and ECM commands must be inside a card transaction");
        if (message[1] != 0x30 && message[1] != 0x36) return FakeCard::transmit(message, response);
        std::unique_lock lock(mutex);
        if (message[1] == 0x30) {
            ++initializations;
            std::vector<uint8_t> data(17, 0);
            data[4] = 0x21;
            data[7] = 5;
            std::copy(id.begin(), id.end(), data.begin() + 8);
            if (shortInit) data.resize(5);
            response = ApduResponse(0x90, 0, data);
            return SCARD_S_SUCCESS;
        }
        ++updates;
        emmEntered = true;
        lastMessage = message;
        cv.notify_all();
        cv.wait(lock, [&] { return !blockEmm; });
        if (resets > 0) { --resets; return SCARD_W_RESET_CARD; }
        std::vector<uint8_t> data(6, 0);
        data[4] = reject ? 0xa1 : 0x21;
        response = ApduResponse(0x90, 0, data);
        return SCARD_S_SUCCESS;
    }
protected:
    void beginTransaction() override { require(!transaction, "card transactions must be serialized"); transaction = true; }
    void endTransaction() override { transaction = false; }
};

static std::vector<uint8_t> emmMessage(uint8_t value = 0x10) {
    return {1, 2, 3, 4, 5, 6, 3, 0, 1, value};
}

class UnblockEmm {
public:
    FakeEmmCard& card;
    ~UnblockEmm() {
        { std::lock_guard lock(card.mutex); card.blockEmm = false; card.block = false; }
        card.cv.notify_all();
    }
};

static void testEmm() {
    // Negative cases intentionally log errors. Capture them here so strict
    // PowerShell runners do not mistake expected diagnostics for test failure.
    struct CaptureErrors {
        std::ostringstream text;
        std::streambuf* previous = std::cerr.rdbuf(text.rdbuf());
        ~CaptureErrors() { std::cerr.rdbuf(previous); }
    } errors;
    const auto path = std::filesystem::temp_directory_path() /
        ("dantto4k-emm-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ini");
    struct RemoveFile { std::filesystem::path path; ~RemoveFile() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{path};
    const auto checkConfig = [&](const std::string& text, bool enabled) {
        { std::ofstream file(path); file << text; }
        require(loadConfig(path.string()).enableEmm == enabled, "INI must enable EMM only with [acas] enableEmm=true");
    };
    checkConfig("[acas]\n", false);
    checkConfig("[acas]\nenableEmm=false\n", false);
    checkConfig("[acas]\nenableEmm=1\n", false);
    checkConfig("[audio]\nenableEmm=true\n", false);
    checkConfig("[acas]\nenableEmm = true\n", true);

    AcasCard card;
    auto fakeCard = std::make_unique<FakeEmmCard>();
    auto* fake = fakeCard.get();
    card.setSmartCard(std::move(fakeCard));
    auto other = emmMessage(); other[0] = 0xff;
    require(card.emm(other) == AcasCard::EmmResult::NotAddressed && fake->updates == 0,
            "another card's EMM must never be transmitted");
    require(card.emm(emmMessage()) == AcasCard::EmmResult::Applied && fake->updates == 1,
            "an addressed EMM must reach the card");
    const auto& apdu = fake->lastMessage;
    require(apdu.size() == 16 && apdu[0] == 0x90 && apdu[1] == 0x36 && apdu[3] == 1 &&
            apdu[4] == 10 && apdu.back() == 0 && std::equal(apdu.begin() + 5, apdu.end() - 1, emmMessage().begin()),
            "EMM must use the standard receive APDU with the complete addressed message");
    auto bad = emmMessage(); bad[6] = 100;
    require(card.emm(bad) == AcasCard::EmmResult::Failed && fake->updates == 1,
            "invalid EMM lengths must not reach the card");
    fake->reject = true;
    require(card.emm(emmMessage()) == AcasCard::EmmResult::Failed,
            "SW=9000 alone must not count as an accepted EMM");
    fake->reject = false;
    fake->resets = 1;
    const auto oldInit = fake->initializations;
    require(card.emm(emmMessage()) == AcasCard::EmmResult::Applied && fake->reconnects == 1 &&
            fake->initializations == oldInit + 1, "a reset must re-read the destination ID before retrying EMM");
    fake->resets = 1;
    fake->replaceOnReconnect = true;
    const auto beforeSwap = fake->updates;
    require(card.emm(emmMessage()) == AcasCard::EmmResult::NotAddressed && fake->updates == beforeSwap + 1,
            "a replaced card must not receive an EMM addressed to the previous card after reconnect");
    auto truncated = std::make_unique<FakeEmmCard>(); auto* shortCard = truncated.get();
    shortCard->shortInit = true;
    card.setSmartCard(std::move(truncated));
    require(card.emm(emmMessage()) == AcasCard::EmmResult::Failed && shortCard->updates == 0,
            "truncated card initialization must prevent EMM writes");

    AcasHandler disabled;
    auto disabledCard = std::make_unique<FakeEmmCard>(); auto* disabledFake = disabledCard.get();
    disabled.setSmartCard(std::move(disabledCard));
    require(!disabled.onEmm(emmMessage()) && disabledFake->initializations == 0,
            "default-disabled EMM must not touch a card");

    AcasHandler handler(true);
    auto enabledCard = std::make_unique<FakeEmmCard>(); fake = enabledCard.get();
    handler.setSmartCard(std::move(enabledCard));
    std::vector<uint8_t> ecm(32, 0x11);
    handler.onEcm(ecm);
    auto current = packet();
    require(handler.decrypt(current), "ECM must work before EMM");
    {
        std::lock_guard lock(fake->mutex);
        fake->blockEmm = true;
    }
    UnblockEmm unblock{*fake};
    require(handler.onEmm(emmMessage()), "enabled EMM must be queued");
    {
        std::unique_lock lock(fake->mutex);
        require(fake->cv.wait_for(lock, std::chrono::seconds(2), [&] { return fake->emmEntered; }),
                "worker must process EMM without blocking the input thread");
    }
    auto keyCheck = packet(MmtTlv::EncryptionFlag::ODD);
    const auto start = std::chrono::steady_clock::now();
    require(handler.decrypt(keyCheck) && std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
            "EMM I/O must not prevent using already-published keys on either parity");
    for (int i = 0; i < 100; ++i) require(handler.onEmm(emmMessage()), "in-flight duplicate EMM is already queued");
    {
        std::lock_guard lock(fake->mutex);
        require(fake->updates == 1, "duplicate in-flight EMM must not be sent again");
    }
    for (int i = 0; i < 64; ++i) require(handler.onEmm(emmMessage(static_cast<uint8_t>(i + 20))), "bounded EMM queue admission");
    require(!handler.onEmm(emmMessage(200)), "EMM queue must be bounded");
    ecm[0]++;
    handler.onEcm(ecm);
    {
        std::lock_guard lock(fake->mutex);
        fake->block = true;
        fake->entered = false;
        fake->blockEmm = false;
    }
    fake->cv.notify_all();
    {
        std::unique_lock lock(fake->mutex);
        require(fake->cv.wait_for(lock, std::chrono::seconds(2), [&] { return fake->entered; }), "ECM priority dispatch");
        require(fake->updates == 1, "ECM must run before the pending EMM backlog");
    }
    const auto cancelStart = std::chrono::steady_clock::now();
    handler.clear();
    require(std::chrono::steady_clock::now() - cancelStart < std::chrono::seconds(1), "clear must cancel EMM backlog without waiting for card I/O");
    {
        std::lock_guard lock(fake->mutex);
        fake->block = false;
    }
    fake->cv.notify_all();
    handler.onEcm(ecm);
    require(handler.decrypt(current), "ECM must recover after EMM backlog cancellation");
    {
        std::lock_guard lock(fake->mutex);
        require(fake->updates == 1, "cancelled queued EMMs must not reach the card");
    }
    require(errors.text.str().find("SW=0x9000, return=0xa100") != std::string::npos &&
            errors.text.str().find("truncated body (5 bytes)") != std::string::npos,
            "EMM failures must identify the card status or truncated response length");
    std::puts("PASS: EMM INI opt-in, card addressing, APDU validation, reset recovery, duplicate suppression, ECM priority and bounded cancellation");
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
        // A parity switch waits for the failed ECM; afterwards neither parity
        // may fall back to the previous key.
        auto badOdd = packet(MmtTlv::EncryptionFlag::ODD);
        const auto originalOdd = badOdd.payload;
        require(!handler.decrypt(badOdd) && badOdd.payload == originalOdd,
                "failed ECM must not decrypt the switched parity");
        auto bad = packet();
        const auto original = bad.payload;
        require(!handler.decrypt(bad) && bad.payload == original,
                "failed ECM must not decrypt with the previous key");
        {
            std::lock_guard<std::mutex> lock(fake->mutex);
            fake->fail = false;
        }
        const auto transmitCount = [&] {
            std::lock_guard<std::mutex> lock(fake->mutex);
            return fake->transmits;
        };
        // Repetitions of the failed ECM within the retry interval stay off the card.
        const auto beforeRepeat = transmitCount();
        for (int i = 0; i < 20; ++i) {
            handler.onEcm(ecm);
            require(!handler.decrypt(bad), "throttled ECM must not publish keys");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(transmitCount() == beforeRepeat, "failed ECM must not be retried before the interval");
        // A different ECM is not throttled.
        auto otherEcm = ecm;
        otherEcm[1]++;
        handler.onEcm(otherEcm);
        require(handler.decrypt(bad) && transmitCount() > beforeRepeat,
                "a new ECM must be sent to the card immediately");
        // After the interval the identical failed ECM is retried.
        {
            std::lock_guard<std::mutex> lock(fake->mutex);
            fake->fail = true;
        }
        ecm[0]++;
        handler.onEcm(ecm);
        require(!handler.decrypt(badOdd), "failed ECM must not publish keys");
        {
            std::lock_guard<std::mutex> lock(fake->mutex);
            fake->fail = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        handler.onEcm(ecm);
        require(handler.decrypt(bad), "identical ECM must retry after the interval");

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
        {
            Unblock unblock{ *fake };
            // The parity in use keeps decrypting while the next ECM is on the card.
            auto current = packet();
            const auto start = std::chrono::steady_clock::now();
            require(handler.decrypt(current), "current parity must keep its key during ECM update");
            require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
                    "current parity must not wait for card I/O");
            // Switching parity needs the new ECM, so it waits for the card.
            auto switched = std::async(std::launch::async, [&] {
                auto next = packet(MmtTlv::EncryptionFlag::ODD);
                return handler.decrypt(next);
            });
            require(switched.wait_for(std::chrono::milliseconds(300)) == std::future_status::timeout,
                    "parity switch must wait for the pending ECM");
            {
                std::lock_guard<std::mutex> lock(fake->mutex);
                fake->block = false;
            }
            fake->cv.notify_all();
            require(switched.wait_for(std::chrono::seconds(2)) == std::future_status::ready && switched.get(),
                    "parity switch must use the new ECM once the card answers");
        }
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
        std::puts("PASS: ECM success, failure, throttled identical retry, non-blocking update, cancellation and generation recovery");
        testEmm();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
