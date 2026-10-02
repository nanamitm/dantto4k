#include "acasHandler.h"
#include "config.h"
#include "mmtp.h"
#include "aes.h"

AcasHandler::AcasHandler() {
    acasCard = std::make_unique<AcasCard>();
    hasAESNI = AESCtrCipher::hasAESNI();
    workerThread = std::thread(&AcasHandler::worker, this);
}

AcasHandler::~AcasHandler() {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        running = false;
    }

    queueCv.notify_one();
    if (workerThread.joinable()) {
        workerThread.join();
    }
}

bool AcasHandler::onEcm(const std::vector<uint8_t>& ecm) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (lastEcm == ecm && ecmReady) {
            return true;
        }
        lastEcm = ecm;
        queue.push({ generation, ecm });
        // Keep the published key: the parity in use stays valid while the
        // next ECM is processed, so decryption must not stall on card I/O.
        ecmReady = true;
    }
    queueCv.notify_one();

    return true;
}

bool AcasHandler::decrypt(MmtTlv::Mmtp& mmtp) {
    if (!mmtp.extensionHeaderScrambling) {
        return false;
    }
    auto key = getDecryptionKey(mmtp.extensionHeaderScrambling->encryptionFlag);
    if (!key) {
        return false;
    }

    std::array<uint8_t, 16> iv{};
    uint16_t packetIdBe = MmtTlv::Common::swapEndian16(mmtp.packetId);
    uint32_t packetSequenceNumberBe = MmtTlv::Common::swapEndian32(mmtp.packetSequenceNumber);
    memcpy(iv.data(), &packetIdBe, 2);
    memcpy(iv.data() + 2, &packetSequenceNumberBe, 4);

    if (mmtp.payload.size() < 8) {
        return false;
    }

    if (hasAESNI) { [[likely]]
        if (lastKey != *key) { [[unlikely]]
            aes.setKey(*key);
            lastKey = *key;
        }
        aes.setIv(iv);
        aes.decrypt(mmtp.payload.data() + 8, static_cast<int>(mmtp.payload.size() - 8), mmtp.payload.data() + 8);
    }
    else {
        struct AES_ctx ctx;
        AES_init_ctx_iv(&ctx, (*key).data(), iv.data());
        AES_CTR_xcrypt_buffer(&ctx, mmtp.payload.data() + 8, static_cast<int>(mmtp.payload.size() - 8));
    }

    return true;
}

void AcasHandler::clear() {
    std::queue<Task> empty;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        ++generation;
        queue.swap(empty);
        ecmReady = false;
        keyValid = false;
        key = {};
        lastKeyType = MmtTlv::EncryptionFlag::UNSCRAMBLED;
        lastEcm.clear();
    }
    // An in-flight card transaction may finish later. Its old generation is
    // ignored by worker(); selecting a channel never waits for card I/O.
    queueCv.notify_all();
}

void AcasHandler::setSmartCard(std::unique_ptr<ISmartCard> sc) {
    acasCard->setSmartCard(std::move(sc));
}

std::optional<std::array<uint8_t, 16>> AcasHandler::getDecryptionKey(MmtTlv::EncryptionFlag keyType) {
    std::unique_lock<std::mutex> lock(queueMutex);
    if (!ecmReady || (keyType != MmtTlv::EncryptionFlag::EVEN &&
                      keyType != MmtTlv::EncryptionFlag::ODD)) {
        return std::nullopt;
    }
    // Only a missing key or a parity switch needs the newest ECM result.
    if (!keyValid || keyType != lastKeyType) {
        const auto requestedGeneration = generation;
        if (!queueCv.wait_for(lock, std::chrono::seconds(10), [&]() {
                return generation != requestedGeneration || (!workerBusy && queue.empty());
            }) || generation != requestedGeneration || !keyValid) {
            return std::nullopt;
        }
    }
    lastKeyType = keyType;
    return keyType == MmtTlv::EncryptionFlag::EVEN ? key.even : key.odd;
}

void AcasHandler::worker() {
    while (true) {
        Task current;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [&]() {
                return !queue.empty() || !running;
            });

            if (!running) {
                break;
            }

            current = std::move(queue.front());
            queue.pop();
            workerBusy = true;
        }

        AcasCard::DecryptionKey key = {};
        const bool ecmOk = acasCard->ecm(current.second, key);

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            if (generation == current.first && queue.empty()) {
                keyValid = ecmOk;
                if (ecmOk) {
                    this->key = key;
                } else {
                    // Allow an identical ECM to retry after a card failure.
                    ecmReady = false;
                    lastEcm.clear();
                }
            }
            workerBusy = false;
        }
        queueCv.notify_all();
    }
}
