#include "acasHandler.h"
#include "config.h"
#include "mmtp.h"
#include "aes.h"
#include <algorithm>

AcasHandler::AcasHandler(bool enableEmm) : enableEmm(enableEmm) {
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
        if (failedEcm == ecm &&
            std::chrono::steady_clock::now() - failedEcmTime < kEcmRetryInterval) {
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

bool AcasHandler::onEmm(const std::vector<uint8_t>& emm) {
    if (!enableEmm || emm.size() < 9 || emm.size() > 255 ||
        emm[6] < 2 || emm.size() != 7u + emm[6]) return false;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        const auto now = std::chrono::steady_clock::now();
        // Once initialized, other subscribers' messages must not crowd our
        // addressed update out of the bounded queue on every repetition.
        if (emmTarget && !std::equal(emmTarget->begin(), emmTarget->end(), emm.begin())) return true;
        std::erase_if(recentEmms, [&](const auto& entry) { return entry.second <= now; });
        if ((activeEmm && activeEmm->first == generation && activeEmm->second == emm) ||
            std::any_of(emmQueue.begin(), emmQueue.end(), [&](const auto& task) { return task.second == emm; }) ||
            std::any_of(recentEmms.begin(), recentEmms.end(), [&](const auto& entry) { return entry.first == emm; })) {
            return true;
        }
        if (emmQueue.size() >= maxPendingEmms) {
            if (!emmQueueFull) std::cerr << "ACAS EMM queue full (64 pending); waiting for the next broadcast repetition" << std::endl;
            emmQueueFull = true;
            return false;
        }
        emmQueue.push_back({generation, emm});
    }
    queueCv.notify_one();
    return true;
}

void AcasHandler::clear() {
    std::queue<Task> empty;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        ++generation;
        queue.swap(empty);
        emmQueue.clear();
        emmTarget.reset();
        recentEmms.clear();
        emmQueueFull = false;
        ecmReady = false;
        keyValid = false;
        key = {};
        failedEcm.clear();
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
        bool isEmm = false;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [&]() {
                return !queue.empty() || !emmQueue.empty() || !running;
            });

            if (!running) {
                break;
            }

            // ECMs always go first. EMM work must not make a parity switch
            // wait for the whole EMM backlog, nor invalidate published keys.
            if (!queue.empty()) {
                current = std::move(queue.front());
                queue.pop();
                workerBusy = true;
            } else {
                current = std::move(emmQueue.front());
                emmQueue.pop_front();
                activeEmm = current;
                emmQueueFull = false;
                isEmm = true;
            }
        }

        if (isEmm) {
            const auto result = acasCard->emm(current.second);
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                activeEmm.reset();
                emmTarget = acasCard->getEmmCardId();
                if (generation == current.first) {
                    const auto lifetime = result == AcasCard::EmmResult::Applied ?
                        std::chrono::seconds(1800) : std::chrono::seconds(5);
                    if (recentEmms.size() >= 128) recentEmms.pop_front();
                    recentEmms.push_back({std::move(current.second), std::chrono::steady_clock::now() + lifetime});
                    if (result == AcasCard::EmmResult::Applied) {
                        failedEcm.clear(); // The same ECM may now be entitled.
                        std::cerr << "ACAS EMM update accepted" << std::endl;
                    }
                }
            }
            queueCv.notify_all();
            continue;
        }

        AcasCard::DecryptionKey key = {};
        const bool ecmOk = acasCard->ecm(current.second, key);

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            emmTarget = acasCard->getEmmCardId();
            if (generation == current.first && queue.empty()) {
                keyValid = ecmOk;
                if (ecmOk) {
                    this->key = key;
                    failedEcm.clear();
                } else {
                    // Allow an identical ECM to retry after a card failure,
                    // but no sooner than kEcmRetryInterval.
                    ecmReady = false;
                    lastEcm.clear();
                    failedEcm = std::move(current.second);
                    failedEcmTime = std::chrono::steady_clock::now();
                }
            }
            workerBusy = false;
        }
        queueCv.notify_all();
    }
}
