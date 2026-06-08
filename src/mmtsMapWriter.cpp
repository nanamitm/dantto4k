#include "mmtsMapWriter.h"

#include "mhAudioComponentDescriptor.h"
#include "mhStreamIdentificationDescriptor.h"
#include "mmtGeneralLocationInfo.h"
#include "mmtStream.h"
#include "mpt.h"
#include "mpuProcessorBase.h"
#include <algorithm>
#include <cstdio>
#include <sstream>

namespace MmtTlv {

std::string MmtsMapWriter::TrackInfo::key() const
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s:%d:%04X:%d:%u:%d",
                  type.c_str(), streamIndex, packetId, componentTag, samplingRate, latm ? 1 : 0);
    return buf;
}

std::string MmtsMapWriter::TrackInfo::line() const
{
    std::ostringstream oss;
    oss << "track type=" << type
        << " streamIndex=" << streamIndex
        << " packetId=0x" << std::hex << std::uppercase << packetId
        << std::dec << " componentTag=" << componentTag;
    if (type == "audio") {
        oss << " rate=" << samplingRate
            << " latm=" << (latm ? 1 : 0);
    }
    return oss.str();
}

bool MmtsMapWriter::open(const std::filesystem::path& mapPath)
{
    close();
    if (mapPath.empty())
        return false;

    std::ofstream test(mapPath, std::ios::binary | std::ios::trunc);
    if (!test)
        return false;
    test.close();

    path = mapPath;
    sourceSize = 0;
    outputOffset = 0;
    currentPacketOffset = 0;
    firstVideoPtsMs = -1;
    lastVideoPtsMs = -1;
    lastSeekPointMs = -1;
    lastRapPointMs = -1;
    lastMptSignature.clear();
    tracksByKey.clear();
    mptChanges.clear();
    rapPoints.clear();
    seekPoints.clear();
    return true;
}

void MmtsMapWriter::close()
{
    if (path.empty())
        return;

    if (sourceSize == 0)
        sourceSize = outputOffset;

    std::ofstream ofs(path, std::ios::binary);
    if (ofs) {
        ofs << "MMTSMAP 1\n";
        ofs << "source_size=" << sourceSize << "\n";
        ofs << "duration_ms=" << ((firstVideoPtsMs >= 0 && lastVideoPtsMs >= firstVideoPtsMs)
            ? (lastVideoPtsMs - firstVideoPtsMs) : 0) << "\n";
        ofs << "first_video_pts_ms=" << firstVideoPtsMs << "\n";
        ofs << "last_video_pts_ms=" << lastVideoPtsMs << "\n";

        for (const auto& [_, track] : tracksByKey)
            ofs << track.line() << "\n";
        for (const auto& line : mptChanges)
            ofs << line << "\n";
        for (const auto& line : rapPoints)
            ofs << line << "\n";
        for (const auto& line : seekPoints)
            ofs << line << "\n";
    }

    path.clear();
}

void MmtsMapWriter::noteOutputPacket(size_t size)
{
    if (path.empty() || size == 0)
        return;

    currentPacketOffset = outputOffset;
    outputOffset += size;
}

long long MmtsMapWriter::ptsToMs(uint64_t pts, const MmtStream& stream)
{
    if (pts == NOPTS_VALUE)
        return -1;

    const auto& tb = stream.getTimeBase();
    if (tb.den <= 0)
        return static_cast<long long>(pts / 90);

    return static_cast<long long>((static_cast<double>(pts) * tb.num * 1000.0) / tb.den);
}

std::string MmtsMapWriter::signatureOf(const std::vector<TrackInfo>& tracks)
{
    std::ostringstream oss;
    for (const auto& track : tracks)
        oss << track.key() << ";";
    return oss.str();
}

std::string MmtsMapWriter::describeTracks(const std::vector<TrackInfo>& tracks, const char* type)
{
    std::ostringstream oss;
    bool any = false;
    for (const auto& track : tracks) {
        if (track.type != type)
            continue;
        if (any)
            oss << ",";
        any = true;
        oss << track.streamIndex
            << ":0x" << std::hex << std::uppercase << track.packetId
            << std::dec << ":" << track.componentTag;
        if (track.type == "audio")
            oss << ":" << track.samplingRate << ":" << (track.latm ? 1 : 0);
    }
    return any ? oss.str() : "-";
}

void MmtsMapWriter::rememberTrack(const TrackInfo& track)
{
    tracksByKey[track.key()] = track;
}

void MmtsMapWriter::rememberTimedPoint(std::vector<std::string>& lines, const char* kind,
                                       long long timeMs, uint64_t offset, long long minGapMs)
{
    if (timeMs < 0)
        return;
    if (lines.size() > 0) {
        long long& last = (kind[0] == 'r') ? lastRapPointMs : lastSeekPointMs;
        if (last >= 0 && timeMs < last + minGapMs)
            return;
        last = timeMs;
    } else {
        if (kind[0] == 'r')
            lastRapPointMs = timeMs;
        else
            lastSeekPointMs = timeMs;
    }

    std::ostringstream oss;
    oss << kind << " time_ms=" << timeMs << " offset=" << offset;
    lines.push_back(oss.str());
}

void MmtsMapWriter::onVideoData(const MmtStream& stream, const MfuData& mfu)
{
    if (path.empty())
        return;

    const long long ptsMs = ptsToMs(mfu.pts, stream);
    if (ptsMs >= 0) {
        if (firstVideoPtsMs < 0)
            firstVideoPtsMs = ptsMs;
        if (mfu.isLastFragment && ptsMs > lastVideoPtsMs)
            lastVideoPtsMs = ptsMs;

        if (mfu.isLastFragment)
            rememberTimedPoint(seekPoints, "seek", ptsMs, currentPacketOffset, 5000);
        if (mfu.keyframe)
            rememberTimedPoint(rapPoints, "rap", ptsMs, currentPacketOffset, 500);
    }

    TrackInfo track;
    track.type = "video";
    track.streamIndex = static_cast<int>(stream.getStreamIndex());
    track.packetId = stream.getPacketId();
    track.componentTag = stream.getComponentTag();
    rememberTrack(track);
}

void MmtsMapWriter::onAudioData(const MmtStream& stream, const MfuData&)
{
    if (path.empty())
        return;

    TrackInfo track;
    track.type = "audio";
    track.streamIndex = static_cast<int>(stream.getStreamIndex());
    track.packetId = stream.getPacketId();
    track.componentTag = stream.getComponentTag();
    track.samplingRate = stream.getSamplingRate();
    track.latm = stream.is22_2chAudio();
    rememberTrack(track);
}

void MmtsMapWriter::onSubtitleData(const MmtStream& stream, const MfuData&)
{
    if (path.empty())
        return;

    TrackInfo track;
    track.type = "subtitle";
    track.streamIndex = static_cast<int>(stream.getStreamIndex());
    track.packetId = stream.getPacketId();
    track.componentTag = stream.getComponentTag();
    rememberTrack(track);
}

void MmtsMapWriter::onMpt(const Mpt& mpt)
{
    if (path.empty())
        return;

    std::vector<TrackInfo> tracks;
    int streamIndex = 0;
    for (const auto& asset : mpt.assets) {
        bool hasPacketLocation = false;
        uint16_t packetId = 0;
        for (const auto& locationInfo : asset.locationInfos) {
            if (locationInfo.locationType == 0) {
                hasPacketLocation = true;
                packetId = locationInfo.packetId;
                break;
            }
        }
        if (!hasPacketLocation)
            continue;

        const bool isSupported =
            asset.assetType == AssetType::hev1 ||
            asset.assetType == AssetType::mp4a ||
            asset.assetType == AssetType::stpp ||
            asset.assetType == AssetType::aapp;
        if (!isSupported)
            continue;

        if (asset.assetType == AssetType::hev1 ||
            asset.assetType == AssetType::mp4a ||
            asset.assetType == AssetType::stpp) {
            TrackInfo track;
            track.type = asset.assetType == AssetType::hev1 ? "video" :
                         asset.assetType == AssetType::mp4a ? "audio" : "subtitle";
            track.streamIndex = streamIndex;
            track.packetId = packetId;

            for (const auto& descriptor : asset.descriptors.list) {
                if (descriptor->getDescriptorTag() == MhStreamIdentificationDescriptor::kDescriptorTag) {
                    const auto* streamId =
                        static_cast<const MhStreamIdentificationDescriptor*>(descriptor.get());
                    track.componentTag = streamId->componentTag;
                } else if (asset.assetType == AssetType::mp4a &&
                           descriptor->getDescriptorTag() == MhAudioComponentDescriptor::kDescriptorTag) {
                    const auto* audio =
                        static_cast<const MhAudioComponentDescriptor*>(descriptor.get());
                    track.componentTag = audio->componentTag;
                    track.samplingRate = audio->getConvertedSamplingRate();
                    track.latm = audio->is22_2chAudio();
                }
            }

            rememberTrack(track);
            tracks.push_back(track);
        }

        ++streamIndex;
    }

    const std::string signature = signatureOf(tracks);
    if (signature.empty() || signature == lastMptSignature)
        return;

    lastMptSignature = signature;
    std::ostringstream oss;
    const long long timeMs = lastVideoPtsMs >= 0 ? lastVideoPtsMs : firstVideoPtsMs;
    oss << "mpt time_ms=" << timeMs
        << " offset=" << currentPacketOffset
        << " audio=" << describeTracks(tracks, "audio")
        << " subtitle=" << describeTracks(tracks, "subtitle");
    mptChanges.push_back(oss.str());
}

} // namespace MmtTlv
