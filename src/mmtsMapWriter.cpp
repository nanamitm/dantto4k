#include "mmtsMapWriter.h"

#include "mhAudioComponentDescriptor.h"
#include "mhStreamIdentificationDescriptor.h"
#include "mmtGeneralLocationInfo.h"
#include "mmtStream.h"
#include "mpt.h"
#include "mpuProcessorBase.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

namespace MmtTlv {

std::string MmtsMapWriter::TrackInfo::key() const
{
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s:%d:%04X:%d",
                  type.c_str(), streamIndex, packetId, componentTag);
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

namespace {

template <typename T>
void writePod(std::ofstream& ofs, T value)
{
    ofs.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

} // namespace

bool MmtsMapWriter::open(const std::filesystem::path& mapPath, Format mapFormat)
{
    close();
    if (mapPath.empty())
        return false;

    std::ofstream test(mapPath, std::ios::binary | std::ios::trunc);
    if (!test)
        return false;
    test.close();

    path = mapPath;
    format = mapFormat;
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
        if (format == Format::Text)
            writeText(ofs);
        else
            writeBinary(ofs);
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

uint8_t MmtsMapWriter::trackTypeCode(const std::string& type)
{
    if (type == "video")
        return 1;
    if (type == "audio")
        return 2;
    if (type == "subtitle")
        return 3;
    return 0;
}

void MmtsMapWriter::rememberTrack(const TrackInfo& track)
{
    tracksByKey[track.key()] = track;
}

void MmtsMapWriter::rememberTimedPoint(std::vector<TimedPoint>& points, char kind,
                                       long long timeMs, uint64_t offset, long long minGapMs)
{
    if (timeMs < 0)
        return;
    if (points.size() > 0) {
        long long& last = (kind == 'r') ? lastRapPointMs : lastSeekPointMs;
        if (last >= 0 && timeMs < last + minGapMs)
            return;
        last = timeMs;
    } else {
        if (kind == 'r')
            lastRapPointMs = timeMs;
        else
            lastSeekPointMs = timeMs;
    }

    points.push_back(TimedPoint{ timeMs, offset });
}

void MmtsMapWriter::writeText(std::ofstream& ofs) const
{
    ofs << "MMTSMAP 1\n";
    ofs << "source_size=" << sourceSize << "\n";
    ofs << "duration_ms=" << ((firstVideoPtsMs >= 0 && lastVideoPtsMs >= firstVideoPtsMs)
        ? (lastVideoPtsMs - firstVideoPtsMs) : 0) << "\n";
    ofs << "first_video_pts_ms=" << firstVideoPtsMs << "\n";
    ofs << "last_video_pts_ms=" << lastVideoPtsMs << "\n";

    for (const auto& [_, track] : tracksByKey)
        ofs << track.line() << "\n";
    for (const auto& change : mptChanges) {
        ofs << "mpt time_ms=" << change.timeMs
            << " offset=" << change.offset
            << " audio=" << describeTracks(change.tracks, "audio")
            << " subtitle=" << describeTracks(change.tracks, "subtitle")
            << "\n";
    }
    for (const auto& point : rapPoints)
        ofs << "rap time_ms=" << point.timeMs << " offset=" << point.offset << "\n";
    for (const auto& point : seekPoints)
        ofs << "seek time_ms=" << point.timeMs << " offset=" << point.offset << "\n";
}

void MmtsMapWriter::writeBinary(std::ofstream& ofs) const
{
    struct BinaryTrack {
        uint8_t type;
        uint8_t flags;
        uint16_t reserved;
        int32_t streamIndex;
        uint16_t packetId;
        int16_t componentTag;
        uint32_t samplingRate;
        uint32_t reserved2;
    };

    const char magic[8] = { 'M', 'M', 'T', 'S', 'M', 'A', 'P', '2' };
    ofs.write(magic, sizeof(magic));
    writePod<uint32_t>(ofs, 2);
    writePod<uint32_t>(ofs, 0);
    writePod<uint64_t>(ofs, sourceSize);
    writePod<int64_t>(ofs, (firstVideoPtsMs >= 0 && lastVideoPtsMs >= firstVideoPtsMs)
        ? (lastVideoPtsMs - firstVideoPtsMs) : 0);
    writePod<int64_t>(ofs, firstVideoPtsMs);
    writePod<int64_t>(ofs, lastVideoPtsMs);
    writePod<uint32_t>(ofs, static_cast<uint32_t>(tracksByKey.size()));
    writePod<uint32_t>(ofs, static_cast<uint32_t>(mptChanges.size()));
    writePod<uint32_t>(ofs, static_cast<uint32_t>(rapPoints.size()));
    writePod<uint32_t>(ofs, static_cast<uint32_t>(seekPoints.size()));

    std::map<std::string, uint32_t> trackIndices;
    uint32_t index = 0;
    for (const auto& [key, track] : tracksByKey) {
        trackIndices[key] = index++;
        BinaryTrack rec{};
        rec.type = trackTypeCode(track.type);
        rec.flags = track.latm ? 1 : 0;
        rec.streamIndex = static_cast<int32_t>(track.streamIndex);
        rec.packetId = track.packetId;
        rec.componentTag = static_cast<int16_t>(track.componentTag);
        rec.samplingRate = track.samplingRate;
        ofs.write(reinterpret_cast<const char*>(&rec), sizeof(rec));
    }

    for (const auto& change : mptChanges) {
        writePod<int64_t>(ofs, change.timeMs);
        writePod<uint64_t>(ofs, change.offset);
        writePod<uint32_t>(ofs, static_cast<uint32_t>(change.tracks.size()));
        for (const auto& track : change.tracks) {
            auto it = trackIndices.find(track.key());
            writePod<uint32_t>(ofs, it != trackIndices.end() ? it->second : UINT32_MAX);
        }
    }

    auto writePoint = [&ofs](const TimedPoint& point) {
        writePod<int64_t>(ofs, point.timeMs);
        writePod<uint64_t>(ofs, point.offset);
    };
    for (const auto& point : rapPoints)
        writePoint(point);
    for (const auto& point : seekPoints)
        writePoint(point);
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
            rememberTimedPoint(seekPoints, 's', ptsMs, currentPacketOffset, 5000);
        if (mfu.keyframe)
            rememberTimedPoint(rapPoints, 'r', ptsMs, currentPacketOffset, 500);
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
    const long long timeMs = lastVideoPtsMs >= 0 ? lastVideoPtsMs : firstVideoPtsMs;
    MptChange change;
    change.timeMs = timeMs;
    change.offset = currentPacketOffset;
    change.tracks = tracks;
    mptChanges.push_back(change);
}

} // namespace MmtTlv
