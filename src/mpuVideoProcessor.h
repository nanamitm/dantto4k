#pragma once
#include "mpuProcessorBase.h"

namespace MmtTlv {

class MpuVideoProcessor : public MpuProcessorTemplate<AssetType::hev1> {
public:
	MfuProcessResult process(MmtStream& mmtStream, const std::vector<uint8_t>& data, FragmentationIndicator fragmentationIndicator) override;
	void clear();

private:
	std::vector<uint8_t> buffer;
	int sliceSegmentCount{0};
	size_t nalUnitSize{0};
	int nalUnitType{0};
	uint64_t pts{NOPTS_VALUE};
	uint64_t dts{NOPTS_VALUE};

};

}