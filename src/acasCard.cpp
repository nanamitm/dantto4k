#include "acasCard.h"
#include <random>
#include <algorithm>
#include <sstream>
#include "config.h"

uint32_t AcasCard::getA0AuthKcl(sha256_t& output) {
    std::default_random_engine engine(std::random_device{}());
    std::uniform_int_distribution<int> distrib(0, 255);

    std::vector<uint8_t> data = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x8A, 0xF7 };
    std::vector<uint8_t> a0init(8);

    for (size_t i = 0; i < 8; ++i) {
        a0init[i] = static_cast<uint8_t>(distrib(engine));
    }

    data.insert(data.end(), a0init.begin(), a0init.end());

    ApduCommand apdu(0x90, 0xA0, 0x00, 0x01);
    ApduResponse response;

    if (!smartCard->isInited()) {
        smartCard->init();
    }
    if (!smartCard->isConnected()) {
        smartCard->connect();
    }

    LONG result = smartCard->transmit(apdu.case4short(data, 0x00), response);
    if (result != SCARD_S_SUCCESS) {
        return result;
    }

    if (!response.isSuccess()) {
        return SCARD_F_INTERNAL_ERROR;
    }

    auto a0data = response.getData();
    std::vector<uint8_t> a0response(a0data.begin() + 0x06, a0data.begin() + 0x06 + 0x08);
    std::vector<uint8_t> a0hash(a0data.begin() + 0x0e, a0data.end());

    std::vector<uint8_t> plainKcl;
    plainKcl.insert(plainKcl.end(), std::begin(masterKey), std::end(masterKey));
    plainKcl.insert(plainKcl.end(), a0init.begin(), a0init.end());
    plainKcl.insert(plainKcl.end(), a0response.begin(), a0response.end());

    sha256_t kcl = SHA256::hash(plainKcl);

    std::vector<uint8_t> plainData;
    plainData.insert(plainData.end(), kcl.begin(), kcl.end());
    plainData.insert(plainData.end(), a0init.begin(), a0init.end());

    sha256_t hash = SHA256::hash(plainData);

    if (!std::equal(hash.begin(), hash.end(), a0hash.begin())) {
        return SCARD_F_INTERNAL_ERROR;
    }

    output = kcl;

    return SCARD_S_SUCCESS;
}

bool AcasCard::ecm(const std::vector<uint8_t>& ecm, DecryptionKey& output) {
    ApduResponse response;
    sha256_t kcl;
    uint32_t retryCount = 0;
    uint32_t retryReconnectCount = 0;
    if (smartCard == nullptr) {
        return false;
    }

    try {
    retry:
        if (cardReset) {
            casModuleId.reset();
        retryReconnect:
            if (retryReconnectCount > 4) {
                return false;
            }

            uint32_t result = smartCard->reconnect();
            if (result != SCARD_S_SUCCESS) {
                ++retryReconnectCount;
                goto retryReconnect;
            }
            cardReset = false;
        }

        if (!smartCard->isInited()) {
            smartCard->init();
        }
        if (!smartCard->isConnected()) {
            smartCard->connect();
        }

        {
            auto scope = smartCard->scopedTransaction();

            uint32_t result = getA0AuthKcl(kcl);
            if (result != SCARD_S_SUCCESS) {
                if (result == SCARD_W_RESET_CARD) {
                    if (retryCount > 4) {
                        return false;
                    }

                    cardReset = true;
                    ++retryCount;
                    goto retry;
                }

                return false;
            }

            ApduCommand apdu(0x90, 0x34, 0x00, 0x01);
            result = smartCard->transmit(apdu.case4short(ecm, 0x00), response);
            if (result != SCARD_S_SUCCESS) {
                if (result == SCARD_W_RESET_CARD) {
                    if (retryCount > 4) {
                        return false;
                    }

                    cardReset = true;
                    ++retryCount;
                    goto retry;
                }

                return false;
            }

            if (!response.isSuccess()) {
                return false;
            }

            auto ecmData = response.getData();
            std::vector<uint8_t> ecmResponse(ecmData.begin() + 0x06, ecmData.end());
            std::vector<uint8_t> ecmInit(ecm.begin() + 0x04, ecm.begin() + 0x04 + 0x17);

            std::vector<uint8_t> plainData;
            plainData.insert(plainData.end(), kcl.begin(), kcl.end());
            plainData.insert(plainData.end(), ecmInit.begin(), ecmInit.end());

            sha256_t hash = SHA256::hash(plainData);

            for (size_t i = 0; i < hash.size(); i++) {
                hash[i] ^= ecmResponse[i];
            }

            std::copy(hash.begin(), hash.begin() + 0x10, output.odd.begin());
            std::copy(hash.begin() + 0x10, hash.begin() + 0x20, output.even.begin());
        }

        return true;
    }
    catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
        cardReset = true;
        casModuleId.reset();
    }

    return false;
}

void AcasCard::setSmartCard(std::unique_ptr<ISmartCard> sc) {
    smartCard = std::move(sc);
    casModuleId.reset();
    cardReset = false;
}

AcasCard::EmmResult AcasCard::emm(const std::vector<uint8_t>& message) {
    const auto fail = [](const std::string& reason) {
        std::cerr << "ACAS EMM update failed: " << reason << std::endl;
        return EmmResult::Failed;
    };
    if (message.size() < 9 || message.size() > 255 ||
        message[6] < 2 || message.size() != 7u + message[6]) {
        return fail("invalid addressed message length");
    }
    if (!smartCard) return fail("no smart card configured");
    if (!cardReset && casModuleId && smartCard->isInited() && smartCard->isConnected() &&
        !std::equal(casModuleId->begin(), casModuleId->end(), message.begin())) {
        return EmmResult::NotAddressed;
    }
    try {
        // A reset invalidates the cached destination ID. Never send an EMM
        // until the ID has been read again from the currently connected card.
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (cardReset) {
                casModuleId.reset();
                const auto result = smartCard->reconnect();
                if (result != SCARD_S_SUCCESS) {
                    std::ostringstream reason;
                    reason << "reconnect returned PC/SC 0x" << std::hex << result;
                    return fail(reason.str());
                }
                cardReset = false;
            }
            if (!smartCard->isInited() && !smartCard->init()) return fail("reader initialization failed");
            if (!smartCard->isConnected()) {
                casModuleId.reset();
                smartCard->connect();
            }
            auto transaction = smartCard->scopedTransaction();
            const auto checkTransport = [&](uint32_t result, const char* command) {
                if (result == SCARD_S_SUCCESS) return true;
                cardReset = true;
                casModuleId.reset();
                if (result != SCARD_W_RESET_CARD) {
                    std::ostringstream reason;
                    reason << command << " returned PC/SC 0x" << std::hex << result;
                    fail(reason.str());
                }
                return false;
            };
            const auto checkResponse = [&](const ApduResponse& response, const char* command) {
                const auto& data = response.getData();
                if (!response.isSuccess() || data.size() < 6 || data[4] != 0x21 || data[5] != 0x00) {
                    std::ostringstream reason;
                    reason << command << " response SW=0x" << std::hex
                           << ((response.getSw1() << 8) | response.getSw2());
                    if (data.size() >= 6) reason << ", return=0x" << ((data[4] << 8) | data[5]);
                    else reason << ", truncated body (" << std::dec << data.size() << " bytes)";
                    fail(reason.str());
                    return false;
                }
                return true;
            };
            if (!casModuleId) {
                ApduResponse response;
                const auto result = smartCard->transmit(ApduCommand(0x90, 0x30, 0, 1).case2short(0), response);
                if (!checkTransport(result, "initial settings")) {
                    if (result == SCARD_W_RESET_CARD) continue;
                    return EmmResult::Failed;
                }
                if (!checkResponse(response, "initial settings")) return EmmResult::Failed;
                const auto& data = response.getData();
                if (data.size() < 17) return fail("initial settings body is truncated (" + std::to_string(data.size()) + " bytes, need 17)");
                if (data[6] != 0 || data[7] != 5) {
                    std::ostringstream reason;
                    reason << "unsupported CA system ID 0x" << std::hex << ((data[6] << 8) | data[7]);
                    return fail(reason.str());
                }
                std::array<uint8_t, 6> id;
                std::copy_n(data.begin() + 8, id.size(), id.begin());
                casModuleId = id;
            }
            if (!std::equal(casModuleId->begin(), casModuleId->end(), message.begin())) {
                return EmmResult::NotAddressed;
            }
            ApduResponse response;
            const auto result = smartCard->transmit(ApduCommand(0x90, 0x36, 0, 1).case4short(message, 0), response);
            if (!checkTransport(result, "EMM receive")) {
                if (result == SCARD_W_RESET_CARD) continue;
                return EmmResult::Failed;
            }
            return checkResponse(response, "EMM receive") ? EmmResult::Applied : EmmResult::Failed;
        }
        return fail("card reset retry limit exceeded");
    } catch (const std::runtime_error& error) {
        cardReset = true;
        casModuleId.reset();
        return fail(error.what());
    }
}
