#include "mpuApplicationProcessor.h"
#include "mmtStream.h"

namespace MmtTlv {

MfuProcessResult MpuApplicationProcessor::process(MmtStream& mmtStream, const std::vector<uint8_t>& data, FragmentationIndicator fragmentationIndicator) {
    Common::ReadStream stream(data);
    size_t size = stream.leftBytes();
    if (size == 0) {
        return MfuProcessResult::accumulating(); // empty unit: nothing to output, nothing lost
    }

    MfuData mfuData;
    mfuData.data.resize(size);
    stream.read(mfuData.data.data(), size);

    mfuData.streamIndex = mmtStream.getStreamIndex();

    return MfuProcessResult::output(std::move(mfuData));
}

}