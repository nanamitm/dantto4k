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
    bool open(const std::filesystem::path& path);
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

    static long long ptsToMs(uint64_t pts, const MmtStream& stream);
    static std::string signatureOf(const std::vector<TrackInfo>& tracks);
    static std::string describeTracks(const std::vector<TrackInfo>& tracks, const char* type);

    void rememberTrack(const TrackInfo& track);
    void rememberTimedPoint(std::vector<std::string>& lines, const char* kind,
                            long long timeMs, uint64_t offset, long long minGapMs);

    std::filesystem::path path;
    uint64_t sourceSize{0};
    uint64_t outputOffset{0};
    uint64_t currentPacketOffset{0};
    long long firstVideoPtsMs{-1};
    long long lastVideoPtsMs{-1};
    long long lastSeekPointMs{-1};
    long long lastRapPointMs{-1};
    std::string lastMptSignature;
    std::map<std::string, TrackInfo> tracksByKey;
    std::vector<std::string> mptChanges;
    std::vector<std::string> rapPoints;
    std::vector<std::string> seekPoints;
};

} // namespace MmtTlv
