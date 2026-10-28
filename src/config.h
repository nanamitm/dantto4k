#pragma once
#include <string>
#include <fstream>

class Config {
public:
    std::string bondriverPath{};
    std::string mmtsDumpPath{};
    std::string smartCardReaderName{};
    std::string casProxyServer{};
    std::string customWinscardDLL{};
    std::string subtitleDebugLogPath{};
    // Face to draw the characters a caption has no code for. Windows only.
    std::string drcsFallbackFont{};
    bool disableADTSConversion{false};
    bool decodeDump{false};  // true: mmtsDumpPath outputs ACAS-decrypted MMT/TLV
    bool enableEmm{false};  // opt in to addressed ACAS entitlement updates
    bool convertResolutionGaiji{true};  // false: keep ARIB STD-B62 4K/8K/22.2ch/... pictograms as-is instead of "[4K]" etc.
    // Carry data broadcasting (ARIB STD-B62) into the TS as private sections.
    bool appDataTransport{true};
    // Seconds before an unchanged data broadcasting item is sent again.
    int appDataItemResendSeconds{10};
};

Config loadConfig(const std::string& filename);

extern Config config;
