#pragma once

#include "demuxerHandler.h"
#include "damt.h"
#include "dcct.h"
#include "ddmt.h"
#include "ecm.h"
#include "mhAit.h"
#include "mhBit.h"
#include "mhCdt.h"
#include "mhEit.h"
#include "mhSdt.h"
#include "mhTot.h"
#include "mpt.h"
#include "nit.h"
#include "ntp.h"
#include "plt.h"

namespace MmtTlv {

class DemuxerTeeHandler : public DemuxerHandler {
public:
    DemuxerTeeHandler(DemuxerHandler& first, DemuxerHandler& second)
        : first(first), second(second) {}

    void onVideoData(const MmtStream& stream, const MfuData& mfu) override;
    void onAudioData(const MmtStream& stream, const MfuData& mfu) override;
    void onSubtitleData(const MmtStream& stream, const MfuData& mfu) override;
    void onApplicationData(const MmtStream& stream, const Mpu& mpu, const DataUnit& dataUnit, const MfuData& mfu) override;

    void onEcm(const Ecm& ecm) override;
    void onMhBit(const MhBit& mhBit) override;
    void onMhAit(const MhAit& mhAit) override;
    void onMhCdt(const MhCdt& mhCdt) override;
    void onMhEit(const MhEit& mhEit) override;
    void onMhSdtActual(const MhSdt& mhSdt) override;
    void onMhTot(const MhTot& mhTot) override;
    void onMpt(const Mpt& mpt) override;
    void onPlt(const Plt& plt) override;
    void onDamt(const Damt& damt) override;
    void onDdmt(const Ddmt& ddmt) override;
    void onDcct(const Dcct& dcct) override;
    void onNit(const Nit& nit) override;
    void onNtp(const NTPv4& ntp) override;
    void onPacketDrop(uint16_t packetId, const MmtStream* stream) override;

private:
    DemuxerHandler& first;
    DemuxerHandler& second;
};

} // namespace MmtTlv
