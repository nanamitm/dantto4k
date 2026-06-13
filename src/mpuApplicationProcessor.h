#pragma once
#include "mpuProcessorBase.h"

namespace MmtTlv {

class MpuApplicationProcessor : public MpuProcessorTemplate<AssetType::aapp> {
public:
	MfuProcessResult process(MmtStream& mmtStream, const std::vector<uint8_t>& data, FragmentationIndicator fragmentationIndicator) override;

};

}