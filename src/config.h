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
    // When true, full-width brackets transmitted as MSZ are rendered at half
    // width to match ARIB font metrics. When false (default), they are forced
    // to NSZ to avoid overlap with general fonts.
    bool aribBracketSquish{false};
};

Config loadConfig(const std::string& filename);

extern Config config;
