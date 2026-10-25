#pragma once
#include <vector>
#include <memory>
#include <optional>
#include "mpuExtendedTimestampDescriptor.h"
#include "mpuTimestampDescriptor.h"
#include "videoComponentDescriptor.h"
#include "mhAudioComponentDescriptor.h"
#include "mpuProcessorBase.h"
#include "additionalAribSubtitleInfo.h"

namespace MmtTlv {

class VideoComponentDescriptor;
class MhAudioComponentDescriptor;

class MmtStream final {
public:
	explicit MmtStream(uint16_t packetId)
		: packetId(packetId) {}

	MmtStream(const MmtStream&) = delete;
    MmtStream& operator=(const MmtStream&) = delete;

	MmtStream(MmtStream&&) = default;
    MmtStream& operator=(MmtStream&&) = default;

	struct TimeBase {
		int num;
		int den;
	};

	std::pair<int64_t, int64_t> getNextPtsDts();
	uint32_t getAuIndex() const { return auIndex; }

	// Access units this stream could not emit, by reason. A consumer reads the
	// delta when the next AU arrives: without it the hole is silent. The first
	// two are raised by getNextPtsDts(), which an MPU sequence that has moved
	// ahead of its timestamp descriptor - a channel boundary, say - makes throw
	// for every AU until the descriptor catches up, unless extrapolateTiming()
	// can place the AU from a neighbouring MPU's entry.
	uint64_t droppedNoTimestamp{0};
	uint64_t droppedAuIndexRange{0};
	uint64_t droppedBadFrame{0};
	// Raised in processMpu, before a fragment reaches the per-asset processor:
	// one for a fragment the validator refuses (its state was cleared by an MPU
	// change, so a middle or last fragment has nothing to continue), one for a
	// data unit that does not parse.
	uint64_t droppedFragment{0};
	uint64_t droppedMalformed{0};
	// Access units timed by walking out from a neighbouring MPU's descriptor
	// instead of being dropped, and the times that was not possible. A refusal
	// reason is one of the guards in extrapolateTiming().
	uint64_t extrapolated{0};
	uint64_t extrapolationRefusedNoAnchor{0};
	uint64_t extrapolationRefusedShape{0};
	uint64_t extrapolationRefusedDistance{0};
	uint64_t droppedTotal() const {
		return droppedNoTimestamp + droppedAuIndexRange + droppedBadFrame +
			droppedFragment + droppedMalformed;
	}
	uint16_t getMpeg2PacketId() const { return outputPacketId ? *outputPacketId : (componentTag == -1 ? 0x200 + streamIndex : 0x100 + componentTag); }
	uint16_t getPacketId() const { return packetId; }
	uint32_t getAssetType() const { return assetType; }
	uint32_t getStreamIndex() const { return streamIndex; }
	int32_t getComponentTag() const { return componentTag; }
	bool getRapFlag() const { return rapFlag; }
	bool is8KVideo() const;
	bool is22_2chAudio() const;
	uint32_t getSamplingRate() const;
	std::optional<std::reference_wrapper<const VideoComponentDescriptor>> getVideoComponentDescriptor() const { return videoComponentDescriptor; }
	std::optional<std::reference_wrapper<const MhAudioComponentDescriptor>> getMhAudioComponentDescriptor() const { return mhAudioComponentDescriptor; }
	const std::vector<MpuTimestampDescriptor::Entry>& getMpuTimestamps() const { return mpuTimestamps; }
	const std::vector<MpuExtendedTimestampDescriptor::Entry>& getMpuExtendedTimestamps() const { return mpuExtendedTimestamps; }
    const TimeBase& getTimeBase() const { return timeBase; }

	const std::optional<AdditionalAribSubtitleInfo>& additionalAribSubtitleInfo() const noexcept {
		return subtitleInfo;
	}

	bool isClosedCaption() const {
		return assetType == AssetType::stpp && componentTag >= 0x30 && componentTag <= 0x37;
	}

	bool isSuperimposition() const {
		return assetType == AssetType::stpp && componentTag >= 0x38 && componentTag <= 0x3F;
	}

private:
	friend class MmtTlvDemuxer;

	const MpuTimestampDescriptor::Entry* findTimestamp(uint32_t mpuSequenceNumber) const;
	const MpuExtendedTimestampDescriptor::Entry* findExtendedTimestamp(uint32_t mpuSequenceNumber) const;
	// Timing for the current auIndex derived from the nearest MPU this stream
	// does hold descriptors for, or nothing if no guard allows it.
	std::optional<std::pair<int64_t, int64_t>> extrapolateTiming();

	uint16_t packetId;
	std::optional<uint16_t> outputPacketId;
	uint32_t assetType{0};
	uint32_t auIndex{0};
	uint32_t streamIndex{0};
	int16_t componentTag{-1};
	bool rapFlag{false};
	std::optional<uint32_t> lastMpuSequenceNumber;
	std::unique_ptr<MpuProcessorBase> mpuProcessor;
	std::optional<VideoComponentDescriptor> videoComponentDescriptor;
	std::optional<MhAudioComponentDescriptor> mhAudioComponentDescriptor;
	std::vector<MpuTimestampDescriptor::Entry> mpuTimestamps;
	std::vector<MpuExtendedTimestampDescriptor::Entry> mpuExtendedTimestamps;
	std::optional<AdditionalAribSubtitleInfo> subtitleInfo;
	TimeBase timeBase{0, 0};

};

}
