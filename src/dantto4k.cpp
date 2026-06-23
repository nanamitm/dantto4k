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
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>

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
    bool writeMmtsMapText{false};
    bool writeMmtsMapOnly{false};
    std::string mmtsMapPath;
    bool probeVideoTimestamps{false};
    bool probeTsVideoPts{false};
    bool probeTsClocks{false};
    bool probeBitrate{false};
    size_t probeSampleLimit{128};
    uint64_t probeBitrateSampleBytes{256ULL * 1024 * 1024};
};

class NullDemuxerHandler : public MmtTlv::DemuxerHandler {
};

class VideoTimestampProbeHandler : public MmtTlv::DemuxerHandler {
public:
    explicit VideoTimestampProbeHandler(MmtTlv::MmtTlvDemuxer& demuxer, size_t sampleLimit)
        : demuxer(demuxer)
        , sampleLimit(sampleLimit)
    {
    }

    void onMpt(const MmtTlv::Mpt&) override
    {
        std::cout << "[MPT]" << std::endl;
        for (const auto& [packetId, stream] : demuxer.mapStream) {
            if (stream.getAssetType() != MmtTlv::AssetType::hev1) {
                continue;
            }

            std::cout
                << "  video packetId=0x" << std::hex << packetId << std::dec
                << " streamIndex=" << stream.getStreamIndex()
                << " componentTag=" << stream.getComponentTag()
                << " is8K=" << (stream.is8KVideo() ? 1 : 0)
                << " timebase=" << stream.getTimeBase().num << "/" << stream.getTimeBase().den
                << " mpuTimestamps=" << stream.getMpuTimestamps().size()
                << " mpuExtended=" << stream.getMpuExtendedTimestamps().size()
                << std::endl;

            if (printedDescriptorStreams.insert(packetId).second) {
                for (const auto& entry : stream.getMpuTimestamps()) {
                    std::cout
                        << "    [TS] mpuSeq=" << entry.mpuSequenceNumber
                        << " presentationTimeUs=" << entry.mpuPresentationTime
                        << std::endl;
                }
                for (const auto& entry : stream.getMpuExtendedTimestamps()) {
                    std::cout
                        << "    [XTS] mpuSeq=" << entry.mpuSequenceNumber
                        << " numOfAu=" << entry.numOfAu
                        << " dtsOffset=" << entry.mpuDecodingTimeOffset
                        << " ptsOffsets=" << entry.ptsOffsets.size()
                        << " dtsPtsOffsets=" << entry.dtsPtsOffsets.size()
                        << std::endl;
                }
            }
        }
    }

    void onVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfuData) override
    {
        auto& count = sampleCountByPacketId[stream.getPacketId()];
        if (count < sampleLimit || ((count % 128) == 0)) {
            std::cout
                << "[MFU] packetId=0x" << std::hex << stream.getPacketId() << std::dec
                << " idx=" << stream.getStreamIndex()
                << " auIndex=" << (stream.getAuIndex() == 0 ? 0 : stream.getAuIndex() - 1)
                << " pts=" << mfuData.pts
                << " dts=" << mfuData.dts
                << " key=" << (mfuData.keyframe ? 1 : 0)
                << " first=" << (mfuData.isFirstFragment ? 1 : 0)
                << " last=" << (mfuData.isLastFragment ? 1 : 0)
                << " size=" << mfuData.data.size()
                << std::endl;
        }
        ++count;
    }

    void onPacketDrop(uint16_t packetId, const MmtTlv::MmtStream* stream) override
    {
        std::cout
            << "[DROP] packetId=0x" << std::hex << packetId << std::dec
            << " knownStream=" << (stream != nullptr ? 1 : 0)
            << std::endl;
    }

private:
    MmtTlv::MmtTlvDemuxer& demuxer;
    size_t sampleLimit;
    std::set<uint16_t> printedDescriptorStreams;
    std::map<uint16_t, size_t> sampleCountByPacketId;
};

// Scans MPU presentation timestamps across the whole file (any video stream)
// to find the earliest/latest presentation time, so the caller can compute
// an accurate average bitrate (fileSize / duration) without needing to know
// the stream's nominal bitrate ahead of time. Used by BonDriver_File_MMTS to
// auto-detect pacing instead of relying on a manually configured BitrateMbps.
class BitrateProbeHandler : public MmtTlv::DemuxerHandler {
public:
    explicit BitrateProbeHandler(MmtTlv::MmtTlvDemuxer& demuxer)
        : demuxer(demuxer)
    {
    }

    void onMpt(const MmtTlv::Mpt&) override
    {
        for (const auto& [packetId, stream] : demuxer.mapStream) {
            if (stream.getAssetType() != MmtTlv::AssetType::hev1) {
                continue;
            }
            for (const auto& entry : stream.getMpuTimestamps()) {
                const uint64_t t = entry.mpuPresentationTime;
                if (t < minPresentationTimeUs) {
                    minPresentationTimeUs = t;
                }
                if (t > maxPresentationTimeUs) {
                    maxPresentationTimeUs = t;
                }
            }
        }
    }

    bool hasData() const
    {
        return minPresentationTimeUs <= maxPresentationTimeUs;
    }

    double getDurationSec() const
    {
        return static_cast<double>(maxPresentationTimeUs - minPresentationTimeUs) / 1'000'000.0;
    }

private:
    MmtTlv::MmtTlvDemuxer& demuxer;
    uint64_t minPresentationTimeUs{ std::numeric_limits<uint64_t>::max() };
    uint64_t maxPresentationTimeUs{ 0 };
};

struct TsProbeState {
    uint16_t pmtPid{0xFFFF};
    uint16_t pcrPid{0xFFFF};
    uint16_t videoPid{0xFFFF};
    uint16_t audioPid{0xFFFF};
    uint8_t videoStreamType{0};
    uint8_t audioStreamType{0};
    size_t videoPesCount{0};
    size_t audioPesCount{0};
    size_t pcrCount{0};
    int64_t prevVideoPts{-1};
    int64_t prevAudioPts{-1};
    int64_t lastVideoPts{-1};
    int64_t lastAudioPts{-1};
    int64_t lastPcr90k{-1};
};

uint16_t ReadTs16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint64_t ReadPts90k(const uint8_t* p)
{
    return
        ((static_cast<uint64_t>((p[0] >> 1) & 0x07)) << 30) |
        (static_cast<uint64_t>(p[1]) << 22) |
        ((static_cast<uint64_t>(p[2] >> 1)) << 15) |
        (static_cast<uint64_t>(p[3]) << 7) |
        (static_cast<uint64_t>(p[4] >> 1));
}

uint64_t ReadPcr90k(const uint8_t* p)
{
    const uint64_t base =
        (static_cast<uint64_t>(p[0]) << 25) |
        (static_cast<uint64_t>(p[1]) << 17) |
        (static_cast<uint64_t>(p[2]) << 9) |
        (static_cast<uint64_t>(p[3]) << 1) |
        (static_cast<uint64_t>(p[4]) >> 7);
    return base;
}

bool IsAudioStreamType(uint8_t streamType)
{
    switch (streamType) {
    case 0x03:
    case 0x04:
    case 0x0F:
    case 0x11:
    case 0x81:
        return true;
    default:
        return false;
    }
}

void ProbePatSection(const uint8_t* section, size_t size, TsProbeState& state)
{
    if (size < 12 || section[0] != 0x00) {
        return;
    }

    const uint16_t sectionLength = static_cast<uint16_t>(((section[1] & 0x0F) << 8) | section[2]);
    if (sectionLength + 3 > size) {
        return;
    }

    for (size_t pos = 8; pos + 4 <= (3 + sectionLength - 4); pos += 4) {
        const uint16_t programNumber = ReadTs16(section + pos);
        const uint16_t pid = static_cast<uint16_t>(((section[pos + 2] & 0x1F) << 8) | section[pos + 3]);
        if (programNumber != 0) {
            state.pmtPid = pid;
            return;
        }
    }
}

void ProbePmtSection(const uint8_t* section, size_t size, TsProbeState& state)
{
    if (size < 16 || section[0] != 0x02) {
        return;
    }

    const uint16_t sectionLength = static_cast<uint16_t>(((section[1] & 0x0F) << 8) | section[2]);
    if (sectionLength + 3 > size) {
        return;
    }

    state.pcrPid = static_cast<uint16_t>(((section[8] & 0x1F) << 8) | section[9]);

    const uint16_t programInfoLength = static_cast<uint16_t>(((section[10] & 0x0F) << 8) | section[11]);
    size_t pos = 12 + programInfoLength;
    const size_t end = 3 + sectionLength - 4;
    while (pos + 5 <= end) {
        const uint8_t streamType = section[pos];
        const uint16_t elementaryPid = static_cast<uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
        const uint16_t esInfoLength = static_cast<uint16_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
        if (streamType == 0x24) {
            state.videoPid = elementaryPid;
            state.videoStreamType = streamType;
        }
        else if (state.audioPid == 0xFFFF && IsAudioStreamType(streamType)) {
            state.audioPid = elementaryPid;
            state.audioStreamType = streamType;
        }
        pos += 5 + esInfoLength;
    }
}

void PrintTsPtsLog(
    const char* tag,
    size_t count,
    uint16_t pid,
    uint64_t pts,
    int64_t delta,
    int64_t lastPcr90k,
    int64_t otherPts90k)
{
    std::cout
        << "[" << tag << "] pes=" << count
        << " pid=0x" << std::hex << pid << std::dec
        << " pts90k=" << pts
        << " delta90k=" << delta;
    if (lastPcr90k >= 0) {
        std::cout << " pcrDiff90k=" << (static_cast<int64_t>(pts) - lastPcr90k);
    }
    if (otherPts90k >= 0) {
        std::cout << " avDiff90k=" << (static_cast<int64_t>(pts) - otherPts90k);
    }
    std::cout
        << " ptsMs=" << std::fixed << std::setprecision(3) << (static_cast<double>(pts) / 90.0)
        << std::defaultfloat
        << std::endl;
}

void ProbeTsPacket(const uint8_t* packet, TsProbeState& state, size_t sampleLimit, bool includeAudioAndPcr)
{
    if (packet[0] != 0x47) {
        return;
    }

    const bool payloadUnitStart = (packet[1] & 0x40) != 0;
    const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
    const uint8_t adaptationControl = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
    if (adaptationControl == 0 || adaptationControl == 2) {
        return;
    }

    size_t pos = 4;
    if (adaptationControl == 3) {
        if (pos >= 188) {
            return;
        }
        const size_t adaptationLength = packet[pos];
        if (pos + 1 + adaptationLength > 188) {
            return;
        }
        if (includeAudioAndPcr && adaptationLength >= 7 && pid == state.pcrPid) {
            const uint8_t adaptationFlags = packet[pos + 1];
            if ((adaptationFlags & 0x10) != 0) {
                const uint64_t pcr90k = ReadPcr90k(packet + pos + 2);
                const int64_t delta = state.lastPcr90k >= 0
                    ? static_cast<int64_t>(pcr90k) - state.lastPcr90k
                    : -1;
                if (state.pcrCount < sampleLimit || ((state.pcrCount % 256) == 0)) {
                    std::cout
                        << "[TS-PCR] count=" << (state.pcrCount + 1)
                        << " pid=0x" << std::hex << pid << std::dec
                        << " pcr90k=" << pcr90k
                        << " delta90k=" << delta;
                    if (state.lastVideoPts >= 0) {
                        std::cout << " videoDiff90k=" << (state.lastVideoPts - static_cast<int64_t>(pcr90k));
                    }
                    if (state.lastAudioPts >= 0) {
                        std::cout << " audioDiff90k=" << (state.lastAudioPts - static_cast<int64_t>(pcr90k));
                    }
                    std::cout
                        << " pcrMs=" << std::fixed << std::setprecision(3) << (static_cast<double>(pcr90k) / 90.0)
                        << std::defaultfloat
                        << std::endl;
                }
                state.lastPcr90k = static_cast<int64_t>(pcr90k);
                ++state.pcrCount;
            }
        }
        pos += 1 + adaptationLength;
    }
    if (pos >= 188) {
        return;
    }

    const uint8_t* payload = packet + pos;
    const size_t payloadSize = 188 - pos;

    if (pid == 0x0000 && payloadUnitStart && payloadSize >= 1) {
        const size_t pointerField = payload[0];
        if (1 + pointerField < payloadSize) {
            ProbePatSection(payload + 1 + pointerField, payloadSize - 1 - pointerField, state);
        }
        return;
    }

    if (pid == state.pmtPid && payloadUnitStart && payloadSize >= 1) {
        const size_t pointerField = payload[0];
        if (1 + pointerField < payloadSize) {
            ProbePmtSection(payload + 1 + pointerField, payloadSize - 1 - pointerField, state);
        }
        return;
    }

    if (!payloadUnitStart || payloadSize < 14) {
        return;
    }

    if (!(payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01)) {
        return;
    }

    const uint8_t streamId = payload[3];
    const bool isVideoPid = pid == state.videoPid;
    const bool isAudioPid = includeAudioAndPcr && pid == state.audioPid;
    if (!isVideoPid && !isAudioPid) {
        return;
    }
    if (isVideoPid && (streamId & 0xF0) != 0xE0) {
        return;
    }
    if (isAudioPid && ((streamId & 0xE0) != 0xC0) && streamId != 0xBD) {
        return;
    }

    const uint8_t ptsDtsFlags = static_cast<uint8_t>((payload[7] >> 6) & 0x03);
    if ((ptsDtsFlags & 0x02) == 0) {
        return;
    }

    const uint64_t pts = ReadPts90k(payload + 9);
    if (isVideoPid) {
        const int64_t delta = state.prevVideoPts >= 0 ? static_cast<int64_t>(pts) - state.prevVideoPts : -1;
        if (state.videoPesCount < sampleLimit || ((state.videoPesCount % 128) == 0)) {
            PrintTsPtsLog("TS-VIDEO", state.videoPesCount + 1, pid, pts, delta, state.lastPcr90k, state.lastAudioPts);
        }
        state.prevVideoPts = static_cast<int64_t>(pts);
        state.lastVideoPts = static_cast<int64_t>(pts);
        ++state.videoPesCount;
        return;
    }

    const int64_t delta = state.prevAudioPts >= 0 ? static_cast<int64_t>(pts) - state.prevAudioPts : -1;
    if (state.audioPesCount < sampleLimit || ((state.audioPesCount % 128) == 0)) {
        PrintTsPtsLog("TS-AUDIO", state.audioPesCount + 1, pid, pts, delta, state.lastPcr90k, state.lastVideoPts);
    }
    state.prevAudioPts = static_cast<int64_t>(pts);
    state.lastAudioPts = static_cast<int64_t>(pts);
    ++state.audioPesCount;
}

int ProbeTsVideoPts(const std::string& path, size_t sampleLimit)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Unable to open TS file: " << path << std::endl;
        return 1;
    }

    TsProbeState state;
    std::array<uint8_t, 188> packet{};
    size_t packetCount = 0;
    while (ifs.read(reinterpret_cast<char*>(packet.data()), static_cast<std::streamsize>(packet.size()))) {
        ProbeTsPacket(packet.data(), state, sampleLimit, false);
        ++packetCount;
    }

    std::cout
        << "[TS-SUMMARY] packets=" << packetCount
        << " pmtPid=0x" << std::hex << state.pmtPid
        << " pcrPid=0x" << state.pcrPid
        << " videoPid=0x" << state.videoPid
        << " streamType=0x" << static_cast<int>(state.videoStreamType)
        << std::dec
        << " pesCount=" << state.videoPesCount
        << std::endl;

    if (state.videoPid == 0xFFFF || state.videoPesCount == 0) {
        std::cerr << "No video PES with PTS found in TS file" << std::endl;
        return 2;
    }

    return 0;
}

int ProbeTsClocks(const std::string& path, size_t sampleLimit)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Unable to open TS file: " << path << std::endl;
        return 1;
    }

    TsProbeState state;
    std::array<uint8_t, 188> packet{};
    size_t packetCount = 0;
    while (ifs.read(reinterpret_cast<char*>(packet.data()), static_cast<std::streamsize>(packet.size()))) {
        ProbeTsPacket(packet.data(), state, sampleLimit, true);
        ++packetCount;
    }

    std::cout
        << "[TS-CLOCK-SUMMARY] packets=" << packetCount
        << " pmtPid=0x" << std::hex << state.pmtPid
        << " pcrPid=0x" << state.pcrPid
        << " videoPid=0x" << state.videoPid
        << " audioPid=0x" << state.audioPid
        << " videoType=0x" << static_cast<int>(state.videoStreamType)
        << " audioType=0x" << static_cast<int>(state.audioStreamType)
        << std::dec
        << " videoPesCount=" << state.videoPesCount
        << " audioPesCount=" << state.audioPesCount
        << " pcrCount=" << state.pcrCount
        << std::endl;

    if (state.videoPid == 0xFFFF || state.videoPesCount == 0) {
        std::cerr << "No video PES with PTS found in TS file" << std::endl;
        return 2;
    }
    if (state.audioPid == 0xFFFF || state.audioPesCount == 0) {
        std::cerr << "No audio PES with PTS found in TS file" << std::endl;
        return 3;
    }
    if (state.pcrPid == 0xFFFF || state.pcrCount == 0) {
        std::cerr << "No PCR found in TS file" << std::endl;
        return 4;
    }

    return 0;
}

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
            ("write-mmtsmap", "Write a binary .mmtsmap sidecar for --decode-mmts output", cxxopts::value<bool>()->default_value("false"))
            ("write-mmtsmap-text", "Write a text .mmtsmap sidecar for --decode-mmts output", cxxopts::value<bool>()->default_value("false"))
            ("write-mmtsmap-only", "Scan input and write only an .mmtsmap sidecar", cxxopts::value<bool>()->default_value("false"))
            ("mmtsmap", "Specify .mmtsmap output path", cxxopts::value<std::string>())
            ("probe-video-timestamps", "Probe MMTS video timestamp descriptors and MFU pts/dts", cxxopts::value<bool>()->default_value("false"))
            ("probe-ts-video-pts", "Probe MPEG-2 TS video PES PTS values", cxxopts::value<bool>()->default_value("false"))
            ("probe-ts-clocks", "Probe MPEG-2 TS PCR and video/audio PES PTS values", cxxopts::value<bool>()->default_value("false"))
            ("probe-bitrate", "Estimate average bitrate (Mbps) from MPU presentation timestamps and print it", cxxopts::value<bool>()->default_value("false"))
            ("probe-bitrate-sample-mb", "With --probe-bitrate, stop after reading this many MB instead of scanning the whole file (0 = no limit, scan to EOF)", cxxopts::value<uint64_t>()->default_value("256"))
            ("probe-sample-limit", "Number of initial MFU samples to print per video stream", cxxopts::value<size_t>()->default_value("128"))
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
        const bool probeVideoTimestamps = result["probe-video-timestamps"].as<bool>();
        const bool probeTsVideoPts = result["probe-ts-video-pts"].as<bool>();
        const bool probeTsClocks = result["probe-ts-clocks"].as<bool>();
        const bool probeBitrate = result["probe-bitrate"].as<bool>();
        const bool probeOnly = probeVideoTimestamps || probeTsVideoPts || probeTsClocks || probeBitrate;
        if (!result.count("listSmartCardReader") &&
            (!result.count("input") || (!writeMmtsMapOnly && !probeOnly && !result.count("output")))) {
            std::cout << options.help() << std::endl;
            std::exit(1);
        }

        args.input = result["input"].as<std::string>();
        args.output = result["output"].as<std::string>();
        args.writeMmtsMapOnly = writeMmtsMapOnly;
        args.probeVideoTimestamps = probeVideoTimestamps;
        args.probeTsVideoPts = probeTsVideoPts;
        args.probeTsClocks = probeTsClocks;
        args.probeBitrate = probeBitrate;
        args.probeSampleLimit = result["probe-sample-limit"].as<size_t>();
        args.probeBitrateSampleBytes = result["probe-bitrate-sample-mb"].as<uint64_t>() * 1024ULL * 1024ULL;

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
            const bool outputRequired = !args.writeMmtsMapOnly && !args.probeVideoTimestamps && !args.probeTsVideoPts && !args.probeTsClocks && !args.probeBitrate;
            if (!result.count("input") || (outputRequired && !result.count("output"))) {
                std::cerr << (outputRequired ? "input and output arguments are required" : "input argument is required") << std::endl;
                std::exit(1);
            }

            if (outputRequired && args.input != "-" && args.input == args.output) {
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
        if (result["write-mmtsmap-text"].count()) {
            args.writeMmtsMapText = result["write-mmtsmap-text"].as<bool>();
            if (args.writeMmtsMapText)
                args.writeMmtsMap = true;
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
        if (args.probeVideoTimestamps) {
            args.noProgress = false;
            args.decodeMmts = false;
            args.writeMmtsMap = false;
            args.writeMmtsMapOnly = false;
            args.output.clear();
        }
        if (args.probeTsVideoPts) {
            args.noProgress = true;
            args.noStats = true;
            args.decodeMmts = false;
            args.writeMmtsMap = false;
            args.writeMmtsMapOnly = false;
            args.output.clear();
        }
        if (args.probeTsClocks) {
            args.noProgress = true;
            args.noStats = true;
            args.decodeMmts = false;
            args.writeMmtsMap = false;
            args.writeMmtsMapOnly = false;
            args.output.clear();
        }
        if (args.probeBitrate) {
            args.noProgress = true;
            args.noStats = true;
            args.decodeMmts = false;
            args.writeMmtsMap = false;
            args.writeMmtsMapOnly = false;
            args.output.clear();
        }
        if (args.writeMmtsMap && !args.decodeMmts) {
            std::cerr << "--write-mmtsmap requires --decode-mmts" << std::endl;
            std::exit(1);
        }
        if (args.probeVideoTimestamps && args.input == "-") {
            std::cerr << "--probe-video-timestamps cannot be used with stdin input" << std::endl;
            std::exit(1);
        }
        if (args.probeTsVideoPts && args.input == "-") {
            std::cerr << "--probe-ts-video-pts cannot be used with stdin input" << std::endl;
            std::exit(1);
        }
        if (args.probeTsClocks && args.input == "-") {
            std::cerr << "--probe-ts-clocks cannot be used with stdin input" << std::endl;
            std::exit(1);
        }
        if (args.probeBitrate && args.input == "-") {
            std::cerr << "--probe-bitrate cannot be used with stdin input" << std::endl;
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
    if (args.probeTsVideoPts) {
        return ProbeTsVideoPts(args.input, args.probeSampleLimit);
    }
    if (args.probeTsClocks) {
        return ProbeTsClocks(args.input, args.probeSampleLimit);
    }
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
    if (args.probeVideoTimestamps || args.writeMmtsMapOnly || args.probeTsVideoPts || args.probeTsClocks || args.probeBitrate) {
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
    VideoTimestampProbeHandler probeHandler(demuxer, args.probeSampleLimit);
    BitrateProbeHandler bitrateProbeHandler(demuxer);
    MmtTlv::MmtsMapWriter mapWriter;
    std::unique_ptr<BufferedOutput> bufferedOutput;
    uint64_t decodeFailedPacketCount = 0;

    if (args.probeVideoTimestamps) {
        demuxer.setDemuxerHandler(probeHandler);
    }
    else if (args.probeBitrate) {
        demuxer.setDemuxerHandler(bitrateProbeHandler);
    }
    else if (args.decodeMmts) {
        if (args.writeMmtsMap) {
            const auto mapFormat = args.writeMmtsMapText
                ? MmtTlv::MmtsMapWriter::Format::Text
                : MmtTlv::MmtsMapWriter::Format::Binary;
            if (!mapWriter.open(args.mmtsMapPath, mapFormat)) {
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
    uint64_t totalConsumed = 0;
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

        totalConsumed += static_cast<uint64_t>(consumed);

        // 巨大な (数十GB の) 8K キャプチャでは、終端の MPU タイムスタンプを得るためだけに
        // ファイル全体を最後までデマルチプレクスするのは非常に遅い。MMT 放送は概ね CBR
        // なので、先頭の一定バイト数だけのサンプルから推定したビットレートでも実用上十分
        // 正確であり、BonDriver_File_MMTS の再生ペーシング用途には全件スキャンは不要。
        if (args.probeBitrate && args.probeBitrateSampleBytes > 0
                && totalConsumed >= args.probeBitrateSampleBytes
                && bitrateProbeHandler.hasData()) {
            break;
        }
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

    if (args.probeBitrate) {
        if (!bitrateProbeHandler.hasData()) {
            std::cerr << "No MPU timestamp data found; cannot estimate bitrate" << std::endl;
            return 1;
        }
        const double durationSec = bitrateProbeHandler.getDurationSec();
        if (durationSec <= 0.0) {
            std::cerr << "Degenerate duration; cannot estimate bitrate" << std::endl;
            return 1;
        }
        // totalConsumed (実際にデマルチプレクスしたバイト数) を使う。サンプル制限が効いて
        // 全件スキャンしていない場合、これはファイル全体ではなくサンプル分のバイト数。
        const double mbps = static_cast<double>(totalConsumed) * 8.0 / durationSec / 1'000'000.0;
        std::cout << "SAMPLE_BYTES=" << totalConsumed << std::endl;
        std::cout << "DURATION_SEC=" << durationSec << std::endl;
        std::cout << "BITRATE_MBPS=" << mbps << std::endl;
    }

    return 0;
}
