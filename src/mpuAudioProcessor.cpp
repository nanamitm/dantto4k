#include "mpuAudioProcessor.h"
#include "mmtStream.h"
#include "adtsConverter.h"

namespace MmtTlv {

// LOAS audioMuxLengthBytes is 13 bits. Bound accumulation before copying.
constexpr size_t MAX_AAC_FRAME_SIZE = 8191;

MfuProcessResult MpuAudioProcessor::process(MmtStream& mmtStream, const std::vector<uint8_t>& data, FragmentationIndicator fragmentationIndicator) {
    const bool first = fragmentationIndicator == FragmentationIndicator::FirstFragment ||
        fragmentationIndicator == FragmentationIndicator::NotFragmented;
    const bool last = fragmentationIndicator == FragmentationIndicator::LastFragment ||
        fragmentationIndicator == FragmentationIndicator::NotFragmented;
    if (first) {
        clear();
        try {
            frameTimestamps = mmtStream.getNextPtsDts();
        }
        catch (const std::out_of_range&) {
            return MfuProcessResult::dropped();
        }
    }
    if (!frameTimestamps || data.size() > MAX_AAC_FRAME_SIZE - aacFragment.size()) {
        clear();
        ++mmtStream.droppedBadFrame;
        return MfuProcessResult::dropped();
    }
    aacFragment.insert(aacFragment.end(), data.begin(), data.end());
    if (!last) {
        return MfuProcessResult::accumulating();
    }

    // A fragment can end inside the LATM header or its length field. Parse only
    // the complete MFU, and never send partial LOAS frames to ADTSConverter/PES.
    // STD-B60 8.2.1: the MFU is the whole AudioMuxElement, so it sets the LOAS
    // length. Trailing bytes (otherData, padding) are legal; a short one is not.
    const auto size = AACUtils::getFrameSize(aacFragment.data(), aacFragment.size());
    if (!size || static_cast<size_t>(*size) > aacFragment.size()) {
        clear();
        ++mmtStream.droppedBadFrame;
        return MfuProcessResult::dropped();
    }
    MfuData result;
    result.streamIndex = mmtStream.getStreamIndex();
    result.pts = frameTimestamps->first;
    result.dts = frameTimestamps->second;
    result.isFirstFragment = true;
    result.isLastFragment = true;
    result.data.reserve(3 + aacFragment.size());
    result.data = {0x56, static_cast<uint8_t>(0xE0 | (aacFragment.size() >> 8)),
        static_cast<uint8_t>(aacFragment.size())};
    result.data.insert(result.data.end(), aacFragment.begin(), aacFragment.end());
    clear();
    return MfuProcessResult::output(std::move(result));
}

void MpuAudioProcessor::clear() {
    frameTimestamps.reset();
    aacFragment.clear();
}

}