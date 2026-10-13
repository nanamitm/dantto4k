#pragma once
#include <memory>
#include "mpuProcessorBase.h"

namespace MmtTlv {

struct MpuData;
class MmtStream;
class Ecm;
class MhBit;
class MhAit;
class MhCdt;
class MhEit;
class MhSdt;
class MhTot;
class Mpt;
class Plt;
class Nit;
class NTPv4;
class Ddmt;
class Damt;
class Dcct;

class DemuxerHandler {
public:
	virtual ~DemuxerHandler() = default;

	// MPU
	virtual void onVideoData(const MmtStream& mmtStream, const MfuData& mfuData) {}
	virtual void onAudioData(const MmtStream& mmtStream, const MfuData& mfuData) {}
	virtual void onSubtitleData(const MmtStream& mmtStream, const MfuData& mfuData) {}
	virtual void onApplicationData(const MmtStream& mmtStream, const Mpu& mpu, const DataUnit& dataUnit, const MfuData& mfuData) {}
	
	// MMT-SI
	virtual void onEcm(const Ecm& ecm) {}
	virtual void onMhBit(const MhBit& mhBit) {}
	virtual void onMhAit(const MhAit& mhBit) {}
	virtual void onMhCdt(const MhCdt& mhCdt) {}
	virtual void onMhEit(const MhEit& mhEit) {}
	virtual void onMhSdtActual(const MhSdt& mhSdt) {}
	virtual void onMhTot(const MhTot& mhTot) {}
	virtual void onMpt(const Mpt& mpt) {}
	virtual void onPlt(const Plt& plt) {}
    virtual void onDamt(const Damt& damt) {}
	virtual void onDdmt(const Ddmt& damt) {}
	virtual void onDcct(const Dcct& damt) {}
	// Raw bytes of an MH-AIT, DDMT, DAMT, DCCT or EMT section that parsed
	// cleanly, with the packet_id it arrived on.
	virtual void onApplicationTableSection(uint16_t packetId, const uint8_t* data, size_t size) {}
	
	// TLV-SI
	virtual void onNit(const Nit& nit) {}

	// IPv6
	virtual void onNtp(const NTPv4& ntp) {}
	
	virtual void onPacketDrop(uint16_t packetId, const MmtTlv::MmtStream* mmtStream) {}

};

}