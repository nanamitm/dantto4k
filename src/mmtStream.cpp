#include "mmtStream.h"
#include "videoComponentDescriptor.h"
#include "mhAudioComponentDescriptor.h"
#include "timebase.h"
#include <algorithm>
#include <cstdlib>

namespace MmtTlv {

std::pair<int64_t, int64_t> MmtStream::getNextPtsDts() {
    if (!lastMpuSequenceNumber || timeBase.num == 0 || timeBase.den == 0) {
        ++droppedNoTimestamp;
        throw std::out_of_range("timestamp is not available");
    }

    const MpuTimestampDescriptor::Entry* ts = findTimestamp(*lastMpuSequenceNumber);
    const MpuExtendedTimestampDescriptor::Entry* ext = findExtendedTimestamp(*lastMpuSequenceNumber);

    if (!ts || !ext || ext->numOfAu == 0) {
        // The descriptor for this MPU is not here. At the start of a file, and
        // again after a seek, the signalling we have parsed so far announces
        // MPUs a few sequence numbers further on than the one being decoded,
        // and the one covering it went by before any asset was mapped. It is
        // never going to arrive, so waiting is not an option - but the
        // neighbouring MPU's entry says where this one sits.
        if (const auto timing = extrapolateTiming()) {
            ++extrapolated;
            ++auIndex;
            return *timing;
        }
        ++droppedNoTimestamp;
        throw std::out_of_range("timestamp is not available");
    }

    if (auIndex >= ext->numOfAu ||
        ext->ptsOffsets.size() < auIndex ||
        ext->dtsPtsOffsets.size() <= auIndex) {
        ++droppedAuIndexRange;
        throw std::out_of_range("au index out of bounds");
    }

    int64_t ptime = av_rescale(ts->mpuPresentationTime, timeBase.den,
        1000000ll * timeBase.num);

    int64_t dts = ptime - ext->mpuDecodingTimeOffset;
    for (uint32_t j = 0; j < auIndex; ++j) {
        dts += ext->ptsOffsets[j];
    }

    int64_t pts = dts + ext->dtsPtsOffsets[auIndex];

    ++auIndex;

    return std::pair<int64_t, int64_t>(pts, dts);
}

// An MPU's access units are evenly spaced and consecutive MPUs are contiguous,
// so when every access unit in the nearest described MPU carries the same
// offset, the whole timeline around it follows from arithmetic: one MPU lasts
// numOfAu * step, and sequence numbers run by one. That makes this exact for a
// stream of fixed-size frames - all ARIB audio - rather than an estimate.
//
// It is deliberately narrow. A varying offset means the MPU duration is not
// predictable, and a distant anchor means a channel boundary or a discontinuity
// could sit in between, where presentation time restarts and the arithmetic
// would produce a confident wrong answer. Both refuse.
std::optional<std::pair<int64_t, int64_t>> MmtStream::extrapolateTiming() {
    constexpr int64_t kMaxMpuDistance = 8;

    const MpuExtendedTimestampDescriptor::Entry* anchorExt = nullptr;
    const MpuTimestampDescriptor::Entry* anchorTs = nullptr;
    int64_t anchorDistance = 0;

    for (const auto& entry : mpuExtendedTimestamps) {
        const int64_t distance = static_cast<int64_t>(entry.mpuSequenceNumber) -
            static_cast<int64_t>(*lastMpuSequenceNumber);
        if (distance == 0 || (anchorExt && std::abs(distance) >= std::abs(anchorDistance)))
            continue;
        const MpuTimestampDescriptor::Entry* ts = findTimestamp(entry.mpuSequenceNumber);
        if (!ts)
            continue;
        anchorExt = &entry;
        anchorTs = ts;
        anchorDistance = distance;
    }

    if (!anchorExt) {
        ++extrapolationRefusedNoAnchor;
        return std::nullopt;
    }
    if (std::abs(anchorDistance) > kMaxMpuDistance) {
        ++extrapolationRefusedDistance;
        return std::nullopt;
    }

    const uint32_t numOfAu = anchorExt->numOfAu;
    if (numOfAu == 0 || anchorExt->ptsOffsets.size() < numOfAu ||
        anchorExt->dtsPtsOffsets.size() < numOfAu || auIndex >= numOfAu) {
        ++extrapolationRefusedShape;
        return std::nullopt;
    }

    const int64_t step = anchorExt->ptsOffsets[0];
    if (step <= 0 || std::any_of(anchorExt->ptsOffsets.begin(),
            anchorExt->ptsOffsets.begin() + numOfAu,
            [step](uint16_t value) { return value != step; })) {
        ++extrapolationRefusedShape;
        return std::nullopt;
    }

    const int64_t anchorBase = av_rescale(anchorTs->mpuPresentationTime, timeBase.den,
        1000000ll * timeBase.num) - anchorExt->mpuDecodingTimeOffset;

    // anchorDistance is positive when the anchor describes a later MPU, which
    // is the case this exists for, so the first term walks backwards.
    const int64_t dts = anchorBase - anchorDistance * static_cast<int64_t>(numOfAu) * step +
        static_cast<int64_t>(auIndex) * step;
    const int64_t pts = dts + anchorExt->dtsPtsOffsets[auIndex];

    return std::pair<int64_t, int64_t>(pts, dts);
}

const MpuTimestampDescriptor::Entry* MmtStream::findTimestamp(uint32_t mpuSequenceNumber) const {
    const auto it = std::find_if(mpuTimestamps.begin(), mpuTimestamps.end(),
        [mpuSequenceNumber](const auto& entry) { return entry.mpuSequenceNumber == mpuSequenceNumber; });
    return it == mpuTimestamps.end() ? nullptr : &*it;
}

const MpuExtendedTimestampDescriptor::Entry* MmtStream::findExtendedTimestamp(uint32_t mpuSequenceNumber) const {
    const auto it = std::find_if(mpuExtendedTimestamps.begin(), mpuExtendedTimestamps.end(),
        [mpuSequenceNumber](const auto& entry) { return entry.mpuSequenceNumber == mpuSequenceNumber; });
    return it == mpuExtendedTimestamps.end() ? nullptr : &*it;
}

bool MmtStream::is8KVideo() const {
    if (!videoComponentDescriptor) {
        return false;
    }

    return videoComponentDescriptor->is8KVideo();
}

bool MmtStream::is22_2chAudio() const {
    if (!mhAudioComponentDescriptor) {
        return false;
    }

    return mhAudioComponentDescriptor->is22_2chAudio();
}

uint32_t MmtStream::getSamplingRate() const {
    if (!mhAudioComponentDescriptor) {
        return 0;
    }

    return mhAudioComponentDescriptor->getConvertedSamplingRate();
}

}
