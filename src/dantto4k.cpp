#include <iostream>
#include "cxxopts.hpp"
#include "stream.h"
#include "remuxerHandler.h"
#include "config.h"
#include "mmtTlvDemuxer.h"
#include "demuxerHandler.h"
#include "mmtsMapWriter.h"
#include "aribUtil.h"
#include "casProxyClient.h"
#include "acasHandler.h"
#include "smartCard.h"
#include "bufferedOutput.h"
#include "progressReporter.h"

#ifdef WIN32
#include <Windows.h>
#endif

namespace {

struct Args {
    std::string input;
    std::string output;
    std::string casProxyHost;
    uint16_t casProxyPort{0};
    std::string smartCardReaderName;
    std::string customWinscardDLL;
    bool disableADTSConversion{false};
    bool decodeMmts{false};
    bool listSmartCardReader{false};
    bool noProgress{false};
    bool noStats{false};
    bool writeMmtsMap{false};
    bool writeMmtsMapOnly{false};
    std::string mmtsMapPath;
};

class NullDemuxerHandler : public MmtTlv::DemuxerHandler {
};

Args parseArguments(int argc, char* argv[]) {
    Args args;

    try {
        cxxopts::Options options("dantto4k", "MMT/TLV to MPEG-2 TS Converter (https://github.com/nekohkr/dantto4k)");

        options.add_options()
            ("input", "Input file ('-' for stdin)", cxxopts::value<std::string>()->default_value(""))
            ("output", "Output file ('-' for stdout)", cxxopts::value<std::string>()->default_value(""))
            ("listSmartCardReader", "List available smart card readers", cxxopts::value<bool>()->default_value("false"))
            ("casProxyServer", "Specify the address of a CasProxyServer", cxxopts::value<std::string>())
            ("smartCardReaderName", "Specify the smart card reader to use", cxxopts::value<std::string>())
#ifdef WIN32
            ("customWinscardDLL", "Specify the path to a winscard.dll", cxxopts::value<std::string>())
#endif
            ("disableADTSConversion", "Disable ADTS conversion", cxxopts::value<bool>()->default_value("false"))
            ("decode-mmts", "Output ACAS-decrypted MMT/TLV instead of MPEG-2 TS", cxxopts::value<bool>()->default_value("false"))
            ("write-mmtsmap", "Write an .mmtsmap sidecar for --decode-mmts output", cxxopts::value<bool>()->default_value("false"))
            ("write-mmtsmap-only", "Scan input and write only an .mmtsmap sidecar", cxxopts::value<bool>()->default_value("false"))
            ("mmtsmap", "Specify .mmtsmap output path", cxxopts::value<std::string>())
            ("no-progress", "Disable progress display", cxxopts::value<bool>()->default_value("false"))
            ("no-stats", "Disable packet statistics", cxxopts::value<bool>()->default_value("false"))
            ("help", "Show help");

        options.parse_positional({ "input", "output" });
        options.positional_help("input [output] ('-' for stdin/stdout)");
        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            std::exit(0);
        }

        const bool writeMmtsMapOnly = result["write-mmtsmap-only"].count() &&
            result["write-mmtsmap-only"].as<bool>();
        if (!result.count("listSmartCardReader") &&
            (!result.count("input") || (!writeMmtsMapOnly && !result.count("output")))) {
            std::cout << options.help() << std::endl;
            std::exit(1);
        }

        args.input = result["input"].as<std::string>();
        args.output = result["output"].as<std::string>();
        args.writeMmtsMapOnly = writeMmtsMapOnly;

        if (result["casProxyServer"].count()) {
            std::string casProxyServer = result["casProxyServer"].as<std::string>();
            if (!casProxyServer.empty()) {
                auto parsed = casproxy::parseAddress(casProxyServer);
                if (!parsed) {
                    std::cerr << "Invalid CasProxyServer address" << std::endl;
                    std::exit(1);
                }
                args.casProxyHost = parsed->first;
                args.casProxyPort = parsed->second;
            }
        }

        if (result["smartCardReaderName"].count()) {
            args.smartCardReaderName = result["smartCardReaderName"].as<std::string>();
        }
        if (result["listSmartCardReader"].count()) {
            args.listSmartCardReader = result["listSmartCardReader"].as<bool>();
        }
#ifdef WIN32
        if (result["customWinscardDLL"].count()) {
            args.customWinscardDLL = result["customWinscardDLL"].as<std::string>();
        }
#endif

        if (!args.listSmartCardReader) {
            if (!result.count("input") || (!args.writeMmtsMapOnly && !result.count("output"))) {
                std::cerr << (args.writeMmtsMapOnly ? "input argument is required" : "input and output arguments are required") << std::endl;
                std::exit(1);
            }

            if (!args.writeMmtsMapOnly && args.input != "-" && args.input == args.output) {
                std::cerr << "Input and output paths cannot be the same" << std::endl;
                std::exit(1);
            }
        }

        if (result["disableADTSConversion"].count()) {
            args.disableADTSConversion = result["disableADTSConversion"].as<bool>();
        }
        if (result["decode-mmts"].count()) {
            args.decodeMmts = result["decode-mmts"].as<bool>();
        }
        if (result["write-mmtsmap"].count()) {
            args.writeMmtsMap = result["write-mmtsmap"].as<bool>();
        }
        if (result["mmtsmap"].count()) {
            args.mmtsMapPath = result["mmtsmap"].as<std::string>();
            args.writeMmtsMap = true;
        }
        if (result["no-progress"].count()) {
            args.noProgress = result["no-progress"].as<bool>();
        }
        if (result["no-stats"].count()) {
            args.noStats = result["no-stats"].as<bool>();
        }

        // Disable progress and stats when using stdin/stdout
        if (args.input == "-" || args.output == "-") {
            args.noProgress = true;
            args.noStats = true;
        }
        if (args.writeMmtsMapOnly) {
            args.writeMmtsMap = true;
            args.decodeMmts = true;
        }
        if (args.writeMmtsMap && !args.decodeMmts) {
            std::cerr << "--write-mmtsmap requires --decode-mmts" << std::endl;
            std::exit(1);
        }
        if (args.writeMmtsMapOnly && args.input == "-") {
            std::cerr << "--write-mmtsmap-only cannot be used with stdin input" << std::endl;
            std::exit(1);
        }
        if (args.writeMmtsMap && !args.writeMmtsMapOnly && args.output == "-") {
            std::cerr << "--write-mmtsmap cannot be used with stdout output" << std::endl;
            std::exit(1);
        }
        if (args.writeMmtsMap && args.mmtsMapPath.empty()) {
            args.mmtsMapPath = (args.writeMmtsMapOnly ? args.input : args.output) + "map";
        }
    }
    catch (const cxxopts::exceptions::exception& e) {
        std::cerr << e.what() << std::endl;
        std::exit(1);
    }

    return args;
}

#ifdef WIN32
std::string getExeConfigPath() {
    char modulePath[MAX_PATH];
    DWORD length = GetModuleFileNameA(nullptr, modulePath, static_cast<DWORD>(sizeof(modulePath)));
    if (length == 0 || length >= sizeof(modulePath)) {
        return "";
    }

    std::string path(modulePath, length);
    size_t dotPos = path.find_last_of('.');
    size_t separatorPos = path.find_last_of("\\/");
    if (dotPos == std::string::npos || (separatorPos != std::string::npos && dotPos < separatorPos)) {
        return path + ".ini";
    }
    return path.substr(0, dotPos) + ".ini";
}

void loadExeConfigIfExists() {
    std::string configPath = getExeConfigPath();
    if (configPath.empty()) {
        return;
    }

    std::ifstream file(configPath);
    if (!file.is_open()) {
        return;
    }
    file.close();

    config = loadConfig(configPath);
}
#endif

void printReaderList(const Args& args) {
    try {
        std::unique_ptr<ISmartCard> smartCard;
        if (args.casProxyHost.empty()) {
            smartCard = std::make_unique<LocalSmartCard>();
        }
        else {
            smartCard = std::make_unique<RemoteSmartCard>(args.casProxyHost, args.casProxyPort);
        }

        smartCard->init();
        auto list = smartCard->getReaders();

        for (const auto& reader : list) {
            std::cerr << " - " << reader << std::endl;
        }
    }
    catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
    }
}

}

int main(int argc, char* argv[]) {
    constexpr size_t chunkSize = 1024 * 1024 * 5; // 5MB

    Args args = parseArguments(argc, argv);
#ifdef WIN32
    try {
        loadExeConfigIfExists();
    }
    catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    if (!args.customWinscardDLL.empty()) {
        config.customWinscardDLL = args.customWinscardDLL;
    }
#endif
    if (args.casProxyHost.empty() && !config.casProxyServer.empty()) {
        auto parsed = casproxy::parseAddress(config.casProxyServer);
        if (!parsed) {
            std::cerr << "Invalid CasProxyServer address" << std::endl;
            return 1;
        }
        args.casProxyHost = parsed->first;
        args.casProxyPort = parsed->second;
    }
    if (args.disableADTSConversion) {
        config.disableADTSConversion = true;
    }

    bool useStdin = (args.input == "-");
    bool useStdout = (args.output == "-");

    if (args.listSmartCardReader) {
        printReaderList(args);
        return 0;
    }

    std::istream* inputStream;
    std::unique_ptr<std::ifstream> inputFs;
    if (useStdin) {
        inputStream = &std::cin;
    }
    else {
        inputFs = std::make_unique<std::ifstream>(args.input, std::ios::binary);
        if (!inputFs->is_open()) {
            std::cerr << "Unable to open input file: " << args.input << std::endl;
            return 1;
        }
        inputStream = inputFs.get();
    }

    uint64_t fileSize = 0;
    if (!useStdin) {
        auto currentPos = inputFs->tellg();
        inputFs->seekg(0, std::ios::end);
        fileSize = inputFs->tellg();
        inputFs->seekg(currentPos);
    }
    ProgressReporter progressReporter(fileSize, !args.noProgress);

    std::ostream* outputStream = nullptr;
    std::unique_ptr<std::ofstream> outputFs;
    if (args.writeMmtsMapOnly) {
        outputStream = nullptr;
    }
    else if (useStdout) {
        outputStream = &std::cout;
    }
    else {
        outputFs = std::make_unique<std::ofstream>(args.output, std::ios::binary);
        if (!outputFs->is_open()) {
            std::cerr << "Unable to open output file: " << args.output << std::endl;
            return 1;
        }
        outputStream = outputFs.get();
    }

    MmtTlv::MmtTlvDemuxer demuxer;
    RemuxerHandler handler(demuxer);
    NullDemuxerHandler nullHandler;
    MmtTlv::MmtsMapWriter mapWriter;
    std::unique_ptr<BufferedOutput> bufferedOutput;
    uint64_t decodeFailedPacketCount = 0;

    if (args.decodeMmts) {
        if (args.writeMmtsMap) {
            if (!mapWriter.open(args.mmtsMapPath)) {
                std::cerr << "Failed to open MMTS map: " << args.mmtsMapPath << std::endl;
                return 1;
            }
            if (args.writeMmtsMapOnly) {
                mapWriter.setSourceSize(fileSize);
            }
        }
        demuxer.setDecodedDumpCallback([&](const uint8_t* data, size_t size) {
            if (mapWriter.isOpen()) {
                mapWriter.noteOutputPacket(size);
            }
            if (!args.writeMmtsMapOnly && outputStream) {
                outputStream->write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            }
        });
        demuxer.setDecodedDumpErrorCallback([&]() {
            decodeFailedPacketCount++;
        });
        if (mapWriter.isOpen()) {
            demuxer.setDemuxerHandler(mapWriter);
        } else {
            demuxer.setDemuxerHandler(nullHandler);
        }
    }
    else {
        if (useStdout) {
            handler.setOutputCallback([&](const uint8_t* data, size_t size) {
                assert(size == 188);
                outputStream->write(reinterpret_cast<const char*>(data), size);
            });
        }
        else {
            bufferedOutput = std::make_unique<BufferedOutput>(*outputStream);
            handler.setOutputCallback([&, bo = bufferedOutput.get()](const uint8_t* data, size_t size) {
                assert(size == 188);
                bo->write(data, size);
            });
        }
        demuxer.setDemuxerHandler(handler);
    }

    try {
        // Create ACAS handler and initialize the smart card
        std::unique_ptr<AcasHandler> acasHandler = std::make_unique<AcasHandler>();
        std::unique_ptr<ISmartCard> smartCard;
        if (args.casProxyHost.empty()) {
            smartCard = std::make_unique<LocalSmartCard>();
        }
        else {
            smartCard = std::make_unique<RemoteSmartCard>(args.casProxyHost, args.casProxyPort);
        }
        
        smartCard->setSmartCardReaderName(args.smartCardReaderName.empty() ? config.smartCardReaderName : args.smartCardReaderName);
        acasHandler->setSmartCard(std::move(smartCard));
        demuxer.setCasHandler(std::move(acasHandler));
    }
    catch (const std::runtime_error& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    std::vector<uint8_t> inputBuffer;
    inputBuffer.reserve(chunkSize * 2);
    while (true) {
        if (!useStdin && inputStream->eof()) {
            break;
        }

        size_t oldSize = inputBuffer.size();
        if (oldSize < chunkSize) {
            inputBuffer.resize(oldSize + chunkSize);
            inputStream->read(reinterpret_cast<char*>(inputBuffer.data() + oldSize), chunkSize);
            std::streamsize bytesRead = inputStream->gcount();
            inputBuffer.resize(oldSize + bytesRead);
        }

        MmtTlv::Common::ReadStream stream(inputBuffer);
        while (!stream.isEof()) {
            MmtTlv::DemuxStatus status = demuxer.demux(stream);

            if (status == MmtTlv::DemuxStatus::NotEnoughBuffer) {
                break;
            }
        }
        
        auto consumed = inputBuffer.size() - stream.leftBytes();
        if (consumed > 0) {
            progressReporter.update(consumed);
        }
        inputBuffer.erase(inputBuffer.begin(), inputBuffer.begin() + consumed);
    }

    progressReporter.finish();
    if (!args.noStats) {
        demuxer.printStatistics();
        if (!args.decodeMmts && handler.getAdtsDropCount() > 0) {
            std::cout << "ADTS conversion drop: " << handler.getAdtsDropCount() << std::endl;
        }
    }
    demuxer.clear();

    if (args.decodeMmts) {
        std::cerr << "MMTS decode failed packets: " << decodeFailedPacketCount << std::endl;
    }
    if (mapWriter.isOpen()) {
        mapWriter.close();
        std::cerr << "MMTS map written: " << args.mmtsMapPath << std::endl;
    }

    return 0;
}
