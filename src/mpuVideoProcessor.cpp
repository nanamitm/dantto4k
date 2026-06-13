#include "mpuVideoProcessor.h"
#include "stream.h"
#include "mmtStream.h"
#include <iostream>

namespace MmtTlv {

constexpr uint8_t IDR_W_RADL = 0x13;
constexpr uint8_t IDR_N_LP   = 0x14;
constexpr uint8_t CRA_NUT    = 0x15;
constexpr uint8_t NAL_AUD    = 0x23;
constexpr uint32_t MAX_NAL_SIZE = 16 * 1024 * 1024;

MfuProcessResult MpuVideoProcessor::process(MmtStream& mmtStream, const std::vector<uint8_t>& data, FragmentationIndicator fragmentationIndicator) {
    Common::ReadStream stream(data);
    MfuData mfuData;

    if (nalUnitSize == 0) {
        buffer.clear(); // Clear leftover data from the previous NAL unit.
        if (stream.leftBytes() < 4) {
            return MfuProcessResult::dropped();
        }

        nalUnitSize = stream.getBe32U();
        if (nalUnitSize > MAX_NAL_SIZE) {
            clear();
            return MfuProcessResult::dropped();
        }
        if (nalUnitSize == 0 || stream.leftBytes() == 0) {
            clear();
            return MfuProcessResult::dropped();
        }

        uint8_t uint8 = stream.peek8U();
        nalUnitType = ((uint8 >> 1) & 0b111111);

        if (nalUnitType == NAL_AUD) {
            std::pair<int64_t, int64_t> ptsDts;
            try {
                ptsDts = mmtStream.getNextPtsDts();
            }
            catch (const std::out_of_range&) {
                clear();
                return MfuProcessResult::dropped();
            }

            pts = ptsDts.first;
            dts = ptsDts.second;
            sliceSegmentCount = 0;
            mfuData.isFirstFragment = true;
        }

        static const uint8_t startCode[4] = { 0x00, 0x00, 0x00, 0x01 };
        buffer.insert(buffer.end(), startCode, startCode + 4);

        if (nalUnitType < 0x20) {
            sliceSegmentCount++;
        }
        if (nalUnitType == IDR_W_RADL || nalUnitType == IDR_N_LP || nalUnitType == CRA_NUT) {
            mfuData.keyframe = true;
        }
    }

    size_t oldSize = buffer.size();
    size_t remain = stream.leftBytes();
    if (nalUnitSize < remain) {
        clear();
        return MfuProcessResult::dropped();
    }

    nalUnitSize -= remain;
    buffer.resize(oldSize + remain);
    stream.read(buffer.data() + oldSize, remain);

    if (pts == NOPTS_VALUE) {
        clear();
        return MfuProcessResult::dropped();
    }

    mfuData.pts = pts;
    mfuData.dts = dts;
    mfuData.streamIndex = mmtStream.getStreamIndex();

    if (sliceSegmentCount >= (mmtStream.is8KVideo() ? 4 : 1)) {
        if (nalUnitSize == 0) {
            mfuData.isLastFragment = true;
        }
    }

    mfuData.data = std::move(buffer);
    return MfuProcessResult::output(std::move(mfuData));
}

void MpuVideoProcessor::clear() {
    buffer.clear();
    sliceSegmentCount = 0;
    nalUnitSize = 0;
    nalUnitType = 0;
    pts = NOPTS_VALUE;
    dts = NOPTS_VALUE;

}

}
