#pragma once
#include "demuxerHandler.h"
#include "b24SubtitleConverter.h"
#include "ttml/drcs.h"
#include "damt.h"
#include <tsduck.h>
#include <optional>
#include <unordered_map>
#include <functional>
#include <string>

namespace StreamType {

constexpr uint8_t VIDEO_MPEG1 = 0x01;
constexpr uint8_t VIDEO_MPEG2 = 0x02;
constexpr uint8_t AUDIO_MPEG1 = 0x03;
constexpr uint8_t AUDIO_MPEG2 = 0x04;
constexpr uint8_t PRIVATE_SECTION = 0x05;
constexpr uint8_t PRIVATE_DATA = 0x06;
constexpr uint8_t ISO_IEC_13818_6_TYPE_D = 0x0d;
constexpr uint8_t AUDIO_AAC = 0x0f;
constexpr uint8_t AUDIO_AAC_LATM = 0x11;
constexpr uint8_t VIDEO_MPEG4 = 0x10;
constexpr uint8_t METADATA = 0x15;
constexpr uint8_t VIDEO_H264 = 0x1b;
constexpr uint8_t VIDEO_HEVC = 0x24;

}

namespace MmtTlv {

class Plt;
class Mpt;
class MhBit;
class MhAit;
class MhCdt;
class MhEit;
class MhSdt;
class MhTot;
class Nit;
class MmtStream;
class MmtTlvDemuxer;
class NTPv4;

}

constexpr uint16_t PCR_PID = 0x01FF;

class RemuxerHandler : public MmtTlv::DemuxerHandler {
public:
	RemuxerHandler(MmtTlv::MmtTlvDemuxer& demuxer)
		: demuxer(demuxer) {
		duck.addStandards(ts::Standards::ISDB);
	}

	// MPU
	void onVideoData(const MmtTlv::MmtStream& mmtStream, const MmtTlv::MfuData& mfuData) override;
	void onAudioData(const MmtTlv::MmtStream& mmtStream, const MmtTlv::MfuData& mfuData) override;
	void onSubtitleData(const MmtTlv::MmtStream& mmtStream, const MmtTlv::MfuData& mfuData) override;

	// MMT-SI
	void onMhBit(const MmtTlv::MhBit& mhCdt) override;
	void onEcm(const MmtTlv::Ecm& ecm) override {}
	void onMhCdt(const MmtTlv::MhCdt& mhCdt) override;
	void onMhEit(const MmtTlv::MhEit& mhEit) override;
	void onMhSdtActual(const MmtTlv::MhSdt& mhSdt) override;
	void onMhTot(const MmtTlv::MhTot& mhTot) override;
	void onMpt(const MmtTlv::Mpt& mpt) override;
	void onPlt(const MmtTlv::Plt& plt) override;

	// TLV-SI
	void onNit(const MmtTlv::Nit& nit) override;

	// IPv6
	void onNtp(const MmtTlv::NTPv4& ntp) override;

	void onPacketDrop(uint16_t packetId, const MmtTlv::MmtStream* mmtStream) override;

public:
	using OutputCallback = std::function<void(const uint8_t*, size_t)>;
	void setOutputCallback(OutputCallback cb);
	// Subtracts this 90kHz value from every emitted PTS/DTS/PCR, so editors can
	// rebase per-segment timestamps into one contiguous output timeline.
	void setPtsOffset(int64_t offset90k) { ptsOffset90k = offset90k; }
	void clear();
	uint64_t getAdtsDropCount() const { return adtsDropCount; }
	// MH-EIT sections that did not fit into a single TS section and were dropped.
	uint64_t getEitSectionDropCount() const { return eitSectionDropCount; }

private:
	enum class PesState {
		Init,
		InFragment,
	};

	void writeStream(const MmtTlv::MmtStream& mmtStream, const MmtTlv::MfuData& mfuData, const std::vector<uint8_t>& data);
	void writePcr(uint64_t pcrBase90k);
	void writeSubtitle(const MmtTlv::MmtStream& mmtStream, const B24SubtitleOutput& subtitle,
		std::optional<uint64_t> pts = std::nullopt);
	void writeCaptionManagementData(uint64_t pts);
	void convertAndWriteSubtitle(const MmtTlv::MmtStream& mmtStream, const std::string& ttml);
	MmtTlv::MmtTlvDemuxer& demuxer;
	OutputCallback outputCallback;
	std::unordered_map<uint16_t, uint16_t> mapService2Pid;
	std::unordered_map<uint16_t, uint8_t> mapCC;
	std::unordered_map<uint16_t, std::vector<uint8_t>> mapPesPendingData;
	std::unordered_map<uint16_t, uint32_t> mapPesPacketIndex;
	std::unordered_map<uint16_t, PesState> mapPesState;
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, arib::ttml::DrcsGlyph>> mapSubtitleDrcsGlyphs;
	std::unordered_map<uint32_t, std::vector<std::string>> mapPendingSubtitleTtml;
	uint64_t adtsDropCount{0};
	uint64_t eitSectionDropCount{0};
	int tsid{-1};
	uint64_t lastPcr{};
	uint64_t lastWrittenPcr{};
	uint64_t lastCaptionManagementDataPts{};
	int64_t ptsOffset90k{0};
	inline static const std::vector<uint8_t> ccis = { 0x43, 0x43, 0x49, 0x53, 0x01, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, };
	ts::DuckContext duck;

};
