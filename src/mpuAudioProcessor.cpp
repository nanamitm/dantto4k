#include "mpuAudioProcessor.h"
#include "mmtStream.h"
#include "adtsConverter.h"
#include <fstream>

namespace MmtTlv {

// The re-muxed LOAS AudioSyncStream header below encodes the frame length in a
// 13-bit audioMuxLengthBytes field (max 8191). Accepting a larger frame would
// truncate that length and corrupt the stream, so cap at the format limit. (The
// previous 10 KiB value allowed 8192-10240 byte frames that would be silently
// mangled.) Frames above this are dropped rather than corrupted.
constexpr int MAX_AAC_FRAME_SIZE = 8191;

std::optional<MfuData> MpuAudioProcessor::process(MmtStream& mmtStream, const std::vector<uint8_t>& data, FragmentationIndicator fragmentationIndicator) {
    aacFragment.insert(aacFragment.end(), data.begin(), data.end());

    MfuData mfuData;
    mfuData.streamIndex = mmtStream.getStreamIndex();

    if (aacFrameSize == 0) {
        auto ret = AACUtils::getFrameSize(aacFragment.data(), aacFragment.size());
        if (!ret) {
            clear();
            return std::nullopt;
        }
        if (*ret > MAX_AAC_FRAME_SIZE) {
            clear();
            return std::nullopt;
        }

        aacFrameSize = *ret;

        if (fragmentationIndicator == FragmentationIndicator::FirstFragment || fragmentationIndicator == FragmentationIndicator::NotFragmented) {
            mfuData.isFirstFragment = true;

            std::pair<int64_t, int64_t> ptsDts;
            try {
                ptsDts = mmtStream.getNextPtsDts();
            }
            catch (const std::out_of_range&) {
                clear();
                return std::nullopt;
            }

            mfuData.pts = ptsDts.first;
            mfuData.dts = ptsDts.second;
        }

        mfuData.data.reserve(3 + aacFrameSize);

        // Write LOAS Header
        mfuData.data = {
            0x56,
            static_cast<uint8_t>(((aacFrameSize >> 8) & 0x1F) | 0xE0),
            static_cast<uint8_t>(aacFrameSize & 0xFF)
        };
    }

    const size_t remainingSize = aacFragment.size() - aacFragmentOffset;
    const size_t copySize = std::min(remainingSize, aacFrameSize);
    aacFrameSize -= copySize;

    mfuData.data.insert(mfuData.data.end(),
        aacFragment.begin() + aacFragmentOffset,
        aacFragment.begin() + aacFragmentOffset + copySize);
    aacFragmentOffset += copySize;

    if (aacFrameSize == 0) {
        mfuData.isLastFragment = true;
        aacFragment.erase(aacFragment.begin(), aacFragment.begin() + aacFragmentOffset);
        aacFragmentOffset = 0;

        if (aacFragment.size() != 0) {
            clear();
            return std::nullopt;
        }
    }

    return mfuData;
}

void MpuAudioProcessor::clear() {
    aacFrameSize = 0;
    aacFragmentOffset = 0;
    aacFragment.clear();
}

}