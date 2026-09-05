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
    bool convertResolutionGaiji{true};  // false: keep ARIB STD-B62 4K/8K/22.2ch/... pictograms as-is instead of "[4K]" etc.
};

Config loadConfig(const std::string& filename);

extern Config config;
