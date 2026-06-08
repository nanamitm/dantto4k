#include "demuxerTeeHandler.h"

namespace MmtTlv {

void DemuxerTeeHandler::onVideoData(const MmtStream& stream, const MfuData& mfu)
{
    first.onVideoData(stream, mfu);
    second.onVideoData(stream, mfu);
}

void DemuxerTeeHandler::onAudioData(const MmtStream& stream, const MfuData& mfu)
{
    first.onAudioData(stream, mfu);
    second.onAudioData(stream, mfu);
}

void DemuxerTeeHandler::onSubtitleData(const MmtStream& stream, const MfuData& mfu)
{
    first.onSubtitleData(stream, mfu);
    second.onSubtitleData(stream, mfu);
}

void DemuxerTeeHandler::onApplicationData(const MmtStream& stream, const Mpu& mpu, const DataUnit& dataUnit, const MfuData& mfu)
{
    first.onApplicationData(stream, mpu, dataUnit, mfu);
    second.onApplicationData(stream, mpu, dataUnit, mfu);
}

void DemuxerTeeHandler::onEcm(const Ecm& ecm)
{
    first.onEcm(ecm);
    second.onEcm(ecm);
}

void DemuxerTeeHandler::onMhBit(const MhBit& mhBit)
{
    first.onMhBit(mhBit);
    second.onMhBit(mhBit);
}

void DemuxerTeeHandler::onMhAit(const MhAit& mhAit)
{
    first.onMhAit(mhAit);
    second.onMhAit(mhAit);
}

void DemuxerTeeHandler::onMhCdt(const MhCdt& mhCdt)
{
    first.onMhCdt(mhCdt);
    second.onMhCdt(mhCdt);
}

void DemuxerTeeHandler::onMhEit(const MhEit& mhEit)
{
    first.onMhEit(mhEit);
    second.onMhEit(mhEit);
}

void DemuxerTeeHandler::onMhSdtActual(const MhSdt& mhSdt)
{
    first.onMhSdtActual(mhSdt);
    second.onMhSdtActual(mhSdt);
}

void DemuxerTeeHandler::onMhTot(const MhTot& mhTot)
{
    first.onMhTot(mhTot);
    second.onMhTot(mhTot);
}

void DemuxerTeeHandler::onMpt(const Mpt& mpt)
{
    first.onMpt(mpt);
    second.onMpt(mpt);
}

void DemuxerTeeHandler::onPlt(const Plt& plt)
{
    first.onPlt(plt);
    second.onPlt(plt);
}

void DemuxerTeeHandler::onDamt(const Damt& damt)
{
    first.onDamt(damt);
    second.onDamt(damt);
}

void DemuxerTeeHandler::onDdmt(const Ddmt& ddmt)
{
    first.onDdmt(ddmt);
    second.onDdmt(ddmt);
}

void DemuxerTeeHandler::onDcct(const Dcct& dcct)
{
    first.onDcct(dcct);
    second.onDcct(dcct);
}

void DemuxerTeeHandler::onNit(const Nit& nit)
{
    first.onNit(nit);
    second.onNit(nit);
}

void DemuxerTeeHandler::onNtp(const NTPv4& ntp)
{
    first.onNtp(ntp);
    second.onNtp(ntp);
}

void DemuxerTeeHandler::onPacketDrop(uint16_t packetId, const MmtStream* stream)
{
    first.onPacketDrop(packetId, stream);
    second.onPacketDrop(packetId, stream);
}

} // namespace MmtTlv
