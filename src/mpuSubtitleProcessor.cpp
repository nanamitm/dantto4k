#include "mpuSubtitleProcessor.h"
#include "mmtStream.h"

namespace MmtTlv {

MfuProcessResult MpuSubtitleProcessor::process(MmtStream& mmtStream, const std::vector<uint8_t>& data, FragmentationIndicator fragmentationIndicator) {
    Common::ReadStream stream(data);

    try {
        // ARIB STD-B60 Table 9-1. These are four 8-bit fields; reading them as
        // two 16-bit ones put subtitle_tag and the sequence number into the
        // subsample number.
        const uint8_t subtitleTag = stream.get8U();
        const uint8_t subtitleSequenceNumber = stream.get8U();
        const uint8_t subsampleNumber = stream.get8U();
        const uint8_t lastSubsampleNumber = stream.get8U();

        uint8_t uint8 = stream.get8U();
        uint8_t dataType = uint8 >> 4;
        uint8_t lengthExtFlag = (uint8 >> 3) & 1;
        uint8_t subsampleInfoListFlag = (uint8 >> 2) & 1;

        uint32_t dataSize;
        if (lengthExtFlag)
            dataSize = stream.getBe32U();
        else
            dataSize = stream.getBe16U();

        if (subsampleNumber == 0 && lastSubsampleNumber > 0 && subsampleInfoListFlag) {
            for (int i = 0; i < lastSubsampleNumber; ++i) {
                stream.skip(1); // subsample_i_data_type (4 bits), reserved (4 bits)

                if (lengthExtFlag) {
                    stream.skip(4); // subsample_i_data_size
                }
                else {
                    stream.skip(2); // subsample_i_data_size
                }
            }
        }

        if (stream.leftBytes() < dataSize) {
            return MfuProcessResult::dropped();
        }

        MfuData mfuData;
        mfuData.data.resize(dataSize);
        stream.read(mfuData.data.data(), dataSize);

        mfuData.streamIndex = mmtStream.getStreamIndex();
        mfuData.subtitleTag = subtitleTag;
        mfuData.subtitleSequenceNumber = subtitleSequenceNumber;
        mfuData.subtitleSubsampleNumber = subsampleNumber;
        mfuData.subtitleLastSubsampleNumber = lastSubsampleNumber;
        mfuData.subtitleDataType = dataType;

        return MfuProcessResult::output(std::move(mfuData));
    }
    catch (const std::out_of_range&) {
        return MfuProcessResult::dropped();
    }
}

}
