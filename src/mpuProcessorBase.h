#pragma once
#include <vector>
#include <optional>
#include <memory>
#include "mmtp.h"
#include "dataUnit.h"
#include "mpu.h"

namespace MmtTlv {

constexpr int32_t makeAssetType(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
	return (a << 24) | (b << 16) | (c << 8) | d;
}

namespace AssetType {
	constexpr int32_t hev1 = makeAssetType('h', 'e', 'v', '1');
	constexpr int32_t mp4a = makeAssetType('m', 'p', '4', 'a');
	constexpr int32_t stpp = makeAssetType('s', 't', 'p', 'p');
	constexpr int32_t aapp = makeAssetType('a', 'a', 'p', 'p');
}

constexpr uint64_t NOPTS_VALUE = 0x8000000000000000;

struct MfuData {
	std::vector<uint8_t> data;
	uint64_t pts{NOPTS_VALUE};
	uint64_t dts{NOPTS_VALUE};
	int streamIndex{};
	bool keyframe{false};
	bool isFirstFragment{false};
	bool isLastFragment{false};
	uint8_t subtitleDataType{0};
	uint16_t subtitleSubsampleNumber{0};
	uint16_t subtitleLastSubsampleNumber{0};
};

class MmtStream;

// Outcome of MpuProcessorBase::process(). The previous std::optional<MfuData>
// could not distinguish "consumed, nothing to output yet" from "data discarded",
// so dropped fragments on a FirstFragment/MiddleFragment were never counted as
// output drops. This three-state result makes the intent explicit.
enum class MfuProcessStatus {
	Output,        // a (possibly partial) MFU is ready in `data`
	Accumulating,  // input consumed, nothing to output yet; no data lost
	Dropped,       // data was discarded (parse error / size cap / missing timestamp)
};

struct MfuProcessResult {
	MfuProcessStatus status{MfuProcessStatus::Accumulating};
	std::optional<MfuData> data{};

	static MfuProcessResult output(MfuData d) {
		return { MfuProcessStatus::Output, std::move(d) };
	}
	static MfuProcessResult accumulating() {
		return { MfuProcessStatus::Accumulating, std::nullopt };
	}
	static MfuProcessResult dropped() {
		return { MfuProcessStatus::Dropped, std::nullopt };
	}
};

class MpuProcessorBase {
public:
	virtual ~MpuProcessorBase() = default;
	virtual MfuProcessResult process(MmtStream& mmtStream, const std::vector<uint8_t>& data, FragmentationIndicator fragmentationIndicator) { return MfuProcessResult::accumulating(); }
	virtual void clear() {}

};

template<uint32_t assetType>
class MpuProcessorTemplate : public MpuProcessorBase {
public:
	virtual ~MpuProcessorTemplate() = default;

	static constexpr uint32_t kAssetType = assetType;

};

}
