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
    bool disableADTSConversion{false};
    bool decodeDump{false};  // true: mmtsDumpPath outputs ACAS-decrypted MMT/TLV
};

Config loadConfig(const std::string& filename);

extern Config config;
