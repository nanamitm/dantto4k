#pragma once
#include "mpuProcessorBase.h"

namespace MmtTlv {

class MpuAudioProcessor : public MpuProcessorTemplate<AssetType::mp4a> {
public:
	MfuProcessResult process(MmtStream& mmtStream, const std::vector<uint8_t>& data, FragmentationIndicator fragmentationIndicator) override;
	void clear() override;

private:
	std::optional<std::pair<int64_t, int64_t>> frameTimestamps;
	std::vector<uint8_t> aacFragment;

};

}