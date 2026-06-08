#pragma once

#include "demuxerHandler.h"
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace MmtTlv {

class MmtStream;
class Mpt;

class MmtsMapWriter : public DemuxerHandler {
public:
    enum class Format {
        Binary,
        Text,
    };

    bool open(const std::filesystem::path& path, Format format = Format::Binary);
    void close();
    bool isOpen() const { return !path.empty(); }

    void setSourceSize(uint64_t size) { sourceSize = size; }
    void noteOutputPacket(size_t size);

    void onVideoData(const MmtStream& stream, const MfuData& mfu) override;
    void onAudioData(const MmtStream& stream, const MfuData& mfu) override;
    void onSubtitleData(const MmtStream& stream, const MfuData& mfu) override;
    void onMpt(const Mpt& mpt) override;

private:
    struct TrackInfo {
        std::string type;
        int streamIndex{-1};
        uint16_t packetId{0};
        int componentTag{-1};
        uint32_t samplingRate{0};
        bool latm{false};

        std::string key() const;
        std::string line() const;
    };
    struct TimedPoint {
        long long timeMs{-1};
        uint64_t offset{0};
    };
    struct MptChange {
        long long timeMs{-1};
        uint64_t offset{0};
        std::vector<TrackInfo> tracks;
    };

    static long long ptsToMs(uint64_t pts, const MmtStream& stream);
    static std::string signatureOf(const std::vector<TrackInfo>& tracks);
    static std::string describeTracks(const std::vector<TrackInfo>& tracks, const char* type);
    static uint8_t trackTypeCode(const std::string& type);

    void rememberTrack(const TrackInfo& track);
    void rememberTimedPoint(std::vector<TimedPoint>& points, char kind,
                            long long timeMs, uint64_t offset, long long minGapMs);
    void writeText(std::ofstream& ofs) const;
    void writeBinary(std::ofstream& ofs) const;

    std::filesystem::path path;
    Format format{Format::Binary};
    uint64_t sourceSize{0};
    uint64_t outputOffset{0};
    uint64_t currentPacketOffset{0};
    long long firstVideoPtsMs{-1};
    long long lastVideoPtsMs{-1};
    long long lastSeekPointMs{-1};
    long long lastRapPointMs{-1};
    std::string lastMptSignature;
    std::map<std::string, TrackInfo> tracksByKey;
    std::vector<MptChange> mptChanges;
    std::vector<TimedPoint> rapPoints;
    std::vector<TimedPoint> seekPoints;
};

} // namespace MmtTlv
