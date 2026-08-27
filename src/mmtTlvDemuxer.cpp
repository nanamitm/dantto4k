#include "dataUnit.h"
#include "ecm.h"
#include "m2SectionMessage.h"
#include "m2ShortSectionMessage.h"
#include "mhCdt.h"
#include "mhEit.h"
#include "mhSdt.h"
#include "mhTot.h"
#include "mhBit.h"
#include "mhAit.h"
#include "mmtStream.h"
#include "mmtTlvDemuxer.h"
#include "mpt.h"
#include "fragmentAssembler.h"
#include "mpuProcessorFactory.h"
#include "nit.h"
#include "paMessage.h"
#include "plt.h"
#include "signalingMessage.h"
#include "stream.h"
#include "mmtTableFactory.h"
#include "tlvTableFactory.h"
#include "demuxerHandler.h"
#include "mhStreamIdentificationDescriptor.h"
#include "videoComponentDescriptor.h"
#include "mhAudioComponentDescriptor.h"
#include "mhDataComponentDescriptor.h"
#include "ipv6.h"
#include "ntp.h"
#include "dataTransmissionMessage.h"
#include "caMessage.h"
#include "damt.h"
#include <algorithm>
#include <ostream>
#include "ddmt.h"
#include "dcct.h"

namespace MmtTlv {

namespace {

// A packet_id can carry more than one independent packet_sequence_number series
// over the life of a recording: at an event boundary the broadcaster may hand an
// asset (typically the caption asset, which is idle between programs) over to a
// different source that keeps its own counter. Such a hand-over shows up as a
// huge jump in either direction, whereas real packet loss on a continuously
// transmitted asset is a small gap. Treat anything beyond this distance as a
// counter resync rather than as lost packets, so the drop statistics are not
// inflated by tens of thousands of phantom drops.
constexpr uint32_t sequenceResyncThreshold = 1000;

// Serialize a complete TLV packet: [0x7F][type][len_hi][len_lo][payload]
std::vector<uint8_t> serializeTlvPacket(uint8_t packetType, const std::vector<uint8_t>& payload)
{
    uint16_t len = static_cast<uint16_t>(payload.size());
    std::vector<uint8_t> packet;
    packet.reserve(4 + payload.size());
    packet.push_back(0x7F);
    packet.push_back(packetType);
    packet.push_back(static_cast<uint8_t>(len >> 8));
    packet.push_back(static_cast<uint8_t>(len & 0xFF));
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

} // anonymous namespace

void MmtTlvDemuxer::setDemuxerHandler(DemuxerHandler& demuxerHandler) {
    this->demuxerHandler = &demuxerHandler;
}

void MmtTlvDemuxer::setCasHandler(std::unique_ptr<CasHandler> handler) {
    casHandler = std::move(handler);
}

void MmtTlvDemuxer::setAssumeDescrambled(bool value) {
    assumeDescrambled = value;
}

DemuxStatus MmtTlvDemuxer::demux(Common::ReadStream& stream) {
    size_t cur = stream.getPos();

    if (stream.leftBytes() < 4) {
        return DemuxStatus::NotEnoughBuffer;
    }

    if (!isValidTlv(stream)) {
        stream.skip(1);
        return DemuxStatus::NotValidTlv;
    }

    if (!tlv.unpack(stream)) {
        stream.seek(cur);
        return DemuxStatus::NotEnoughBuffer;
    }

    if (stream.leftBytes() < tlv.getDataLength()) {
        stream.seek(cur);
        return DemuxStatus::NotEnoughBuffer;
    }

    statistics.tlvPacketCount++;

    Common::ReadStream tlvDataStream(tlv.getData());

    try {
        switch (tlv.getPacketType()) {
        case TlvPacketType::Ipv4Packet:
        {
            statistics.tlvIpv4PacketCount++;
            // Pass through as-is to decoded dump.
            if (decodedDumpCallback) {
                auto packet = serializeTlvPacket(static_cast<uint8_t>(TlvPacketType::Ipv4Packet), tlv.getData());
                decodedDumpCallback(packet.data(), packet.size());
            }
            break;
        }
        case TlvPacketType::Ipv6Packet:
        {
            statistics.tlvIpv6PacketCount++;
            // Pass through as-is to decoded dump. These packets are never
            // scrambled, so there is nothing to decrypt - but they carry the
            // NTP clock, and dropping them left a dumped stream with no time
            // reference at all: onNtp() never fires when the dump is read back,
            // so caption timing and PCR generation lose their anchor.
            if (decodedDumpCallback) {
                auto packet = serializeTlvPacket(static_cast<uint8_t>(TlvPacketType::Ipv6Packet), tlv.getData());
                decodedDumpCallback(packet.data(), packet.size());
            }

            IPv6Header ipv6(false);
            if (!ipv6.unpack(tlvDataStream)) {
                break;
            }

            if (ipv6.nexthdr == IPv6::PROTOCOL_UDP) {
                UDPHeader udpHeader;
                if (!udpHeader.unpack(tlvDataStream)) {
                    break;
                }

                // NTP
                if (udpHeader.destination_port == IPv6::PORT_NTP) {
                    NTPv4 ntp;
                    if (!ntp.unpack(tlvDataStream)) {
                        break;
                    }

                    demuxerHandler->onNtp(ntp);
                }
            }
            break;
        }
        case TlvPacketType::HeaderCompressedIpPacket:
        {
            statistics.tlvHeaderCompressedIpPacketCount++;

            // A packet we cannot parse is still a packet the tuner delivered.
            // Pass it through untouched rather than dropping it, so a dump
            // stays a faithful record even when the input is damaged.
            if (!compressedIPPacket.unpack(tlvDataStream)) {
                if (decodedDumpCallback) {
                    auto packet = serializeTlvPacket(static_cast<uint8_t>(TlvPacketType::HeaderCompressedIpPacket), tlv.getData());
                    decodedDumpCallback(packet.data(), packet.size());
                }
                break;
            }

            if (!mmtp.unpack(tlvDataStream)) {
                if (decodedDumpCallback) {
                    auto packet = serializeTlvPacket(static_cast<uint8_t>(TlvPacketType::HeaderCompressedIpPacket), tlv.getData());
                    decodedDumpCallback(packet.data(), packet.size());
                }
                break;
            }

            auto mmtStat = statistics.getMmtStat(mmtp.packetId);
            if (mmtStat->count == 0) {
                mmtStat->lastPacketSequenceNumber = mmtp.packetSequenceNumber;
                mmtStat->count++;
            }
            else {
                const uint32_t expected = mmtStat->lastPacketSequenceNumber + 1;
                if (expected != mmtp.packetSequenceNumber) {
                    const uint32_t distance = mmtp.packetSequenceNumber > expected ?
                        mmtp.packetSequenceNumber - expected :
                        expected - mmtp.packetSequenceNumber;

                    // Only count as lost packets when the gap is plausible for a
                    // continuously transmitted asset; a distant sequence number
                    // means the series itself changed (see sequenceResyncThreshold).
                    if (distance <= sequenceResyncThreshold) {
                        if (mmtp.packetSequenceNumber > expected) {
                            mmtStat->drop += mmtp.packetSequenceNumber - expected;
                        } else {
                            mmtStat->drop++;
                        }
                    }

                    auto mmtStream = getStream(mmtp.packetId);
                    if (mmtStream) {
                        mmtStream->mpuProcessor->clear();
                        auto validator = getFragmentValidator(mmtp.packetId);
                        validator->clear();
                        auto assembler = getAssembler(mmtp.packetId);
                        assembler->clear();
                    }

                    if (demuxerHandler) {
                        demuxerHandler->onPacketDrop(mmtp.packetId, mmtStream);
                    }
                }
                mmtStat->lastPacketSequenceNumber = mmtp.packetSequenceNumber;
                mmtStat->count++;
            }

            bool decrypted = false;
            if (mmtp.extensionHeaderScrambling.has_value()) {
                if (mmtp.extensionHeaderScrambling->encryptionFlag == EncryptionFlag::ODD ||
                    mmtp.extensionHeaderScrambling->encryptionFlag == EncryptionFlag::EVEN) {
                    if (!assumeDescrambled) {
                        if (!casHandler) {
                            if (decodedDumpErrorCallback) {
                                decodedDumpErrorCallback();
                            }
                            if (decodedDumpCallback) {
                                auto packet = serializeTlvPacket(static_cast<uint8_t>(TlvPacketType::HeaderCompressedIpPacket), tlv.getData());
                                decodedDumpCallback(packet.data(), packet.size());
                            }
                            return DemuxStatus::WattingForEcm;
                        }
                        if (!casHandler->decrypt(mmtp)) {
                            mmtStat->outputDrop++;
                            if (decodedDumpErrorCallback) {
                                decodedDumpErrorCallback();
                            }
                            if (decodedDumpCallback) {
                                auto packet = serializeTlvPacket(static_cast<uint8_t>(TlvPacketType::HeaderCompressedIpPacket), tlv.getData());
                                decodedDumpCallback(packet.data(), packet.size());
                            }
                            return DemuxStatus::WattingForEcm;
                        }
                        decrypted = true;
                    }
                }
            }

            // Write the packet to the decoded dump. Rebuilding it from the
            // parsed structures silently drops everything the parser does not
            // model - reserved bits, unknown extension header types, trailing
            // bytes - so patch the bytes the tuner delivered instead.
            // Decryption is AES-CTR and length preserving, and the MMTP payload
            // always runs to the end of the TLV packet, so the plaintext drops
            // straight back into place. The scrambling flag is cleared only when
            // we really did decrypt, never when assumeDescrambled skipped it.
            if (decodedDumpCallback) {
                if (decrypted) {
                    std::vector<uint8_t> patched = tlv.getData();
                    if (patched.size() >= mmtp.payload.size()) {
                        const size_t payloadOffset = patched.size() - mmtp.payload.size();
                        std::copy(mmtp.payload.begin(), mmtp.payload.end(), patched.begin() + payloadOffset);

                        if (mmtp.extensionHeaderFlag && mmtp.extensionHeaderLength >= 5 &&
                            payloadOffset >= mmtp.extensionHeaderLength) {
                            // Extension header field layout:
                            // [2B sub-type][2B reserved][1B flags, encryption flag at bits 4:3]
                            patched[payloadOffset - mmtp.extensionHeaderLength + 4] &= ~0x18u;
                        }
                    }
                    auto packet = serializeTlvPacket(static_cast<uint8_t>(TlvPacketType::HeaderCompressedIpPacket), patched);
                    decodedDumpCallback(packet.data(), packet.size());
                }
                else {
                    auto packet = serializeTlvPacket(static_cast<uint8_t>(TlvPacketType::HeaderCompressedIpPacket), tlv.getData());
                    decodedDumpCallback(packet.data(), packet.size());
                }
            }

            Common::ReadStream mmtpPayloadStream(mmtp.payload);
            switch (mmtp.payloadType) {
            case PayloadType::Mpu:
                processMpu(mmtpPayloadStream);
                break;
            case PayloadType::ContainsOneOrMoreControlMessage:
                processSignalingMessages(mmtpPayloadStream);
                break;
            default:
                break;
            }
            break;
        }
        case TlvPacketType::TransmissionControlSignalPacket:
        {
            statistics.tlvTransmissionControlSignalPacketCount++;
            // Pass through as-is to decoded dump.
            if (decodedDumpCallback) {
                auto packet = serializeTlvPacket(static_cast<uint8_t>(TlvPacketType::TransmissionControlSignalPacket), tlv.getData());
                decodedDumpCallback(packet.data(), packet.size());
            }
            processTlvTable(tlvDataStream);
            break;
        }
        case TlvPacketType::NullPacket:
        {
            statistics.tlvNullPacketCount++;
            // Pass through as-is to decoded dump.
            if (decodedDumpCallback) {
                auto packet = serializeTlvPacket(static_cast<uint8_t>(TlvPacketType::NullPacket), tlv.getData());
                decodedDumpCallback(packet.data(), packet.size());
            }
            break;
        }
        default:
        {
            statistics.tlvUndefinedCount++;
            // Unknown to us, but still part of what the tuner delivered.
            if (decodedDumpCallback) {
                auto packet = serializeTlvPacket(static_cast<uint8_t>(tlv.getPacketType()), tlv.getData());
                decodedDumpCallback(packet.data(), packet.size());
            }
        }
        }
    }
    catch (const std::out_of_range&) {
        return DemuxStatus::Error;
    }

    return DemuxStatus::Ok;
}

void MmtTlvDemuxer::processPaMessage(Common::ReadStream& stream) {
    PaMessage message;
    if (!message.unpack(stream)) {
        return;
    }

    Common::ReadStream nstream(message.table);
    while (!nstream.isEof()) {
        processMmtTable(nstream);
    }
}

void MmtTlvDemuxer::processM2SectionMessage(Common::ReadStream& stream) {
    M2SectionMessage message;
    if (!message.unpack(stream)) {
        return;
    }

    processMmtTable(stream);
}

void MmtTlvDemuxer::processCaMessage(Common::ReadStream& stream) {
    CaMessage message;
    if (!message.unpack(stream)) {
        return;
    }
    
    processMmtTable(stream);
}

void MmtTlvDemuxer::processM2ShortSectionMessage(Common::ReadStream& stream) {
    M2ShortSectionMessage message;
    if (!message.unpack(stream)) {
        return;
    }

    processMmtTable(stream);
}

void MmtTlvDemuxer::processDataTransmissionMessage(Common::ReadStream& stream) {
    DataTransmissionMessage message;
    if (!message.unpack(stream)) {
        return;
    }
    
    processMmtTable(stream);
}

void MmtTlvDemuxer::processTlvTable(Common::ReadStream& stream) {
    if (stream.leftBytes() < 2) {
        return;
    }

    uint8_t tableId = stream.peek8U();
    const auto table = TlvTableFactory::create(tableId);
    if (table == nullptr) {
        return;
    }

    if (!table->unpack(stream)) {
        return;
    }

    switch (tableId) {
    case TlvTableId::Nit:
        if (demuxerHandler) {
            demuxerHandler->onNit(*static_cast<Nit*>(table.get()));
        }
        break;
    }
}

void MmtTlvDemuxer::processMmtTable(Common::ReadStream& stream) {
    uint8_t tableId = stream.peek8U();
    processMmtTableStatistics(tableId);

    const auto table = MmtTableFactory::create(tableId);
    if (table == nullptr) {
        stream.skip(stream.leftBytes());
        return;
    }

    table->unpack(stream);

    switch (tableId) {
    case MmtTableId::Mpt:
        processMmtPackageTable(*static_cast<Mpt*>(table.get()));
        break;
    case MmtTableId::Ecm_0:
        processEcm(*static_cast<Ecm*>(table.get()));
        break;
    }
    
    if (demuxerHandler) {
        switch (tableId) {
        case MmtTableId::Ecm_0:
            demuxerHandler->onEcm(*dynamic_cast<Ecm*>(table.get()));
            break;
        case MmtTableId::MhCdt:
            demuxerHandler->onMhCdt(*dynamic_cast<MhCdt*>(table.get()));
            break;
        case MmtTableId::MhEitPf:
        case MmtTableId::MhEitS_0:
        case MmtTableId::MhEitS_1:
        case MmtTableId::MhEitS_2:
        case MmtTableId::MhEitS_3:
        case MmtTableId::MhEitS_4:
        case MmtTableId::MhEitS_5:
        case MmtTableId::MhEitS_6:
        case MmtTableId::MhEitS_7:
        case MmtTableId::MhEitS_8:
        case MmtTableId::MhEitS_9:
        case MmtTableId::MhEitS_10:
        case MmtTableId::MhEitS_11:
        case MmtTableId::MhEitS_12:
        case MmtTableId::MhEitS_13:
        case MmtTableId::MhEitS_14:
        case MmtTableId::MhEitS_15:
            demuxerHandler->onMhEit(*dynamic_cast<MhEit*>(table.get()));
            break;
        case MmtTableId::MhSdtActual:
            demuxerHandler->onMhSdtActual(*dynamic_cast<MhSdt*>(table.get()));
            break;
        case MmtTableId::MhTot:
            demuxerHandler->onMhTot(*dynamic_cast<MhTot*>(table.get()));
            break;
        case MmtTableId::Mpt:
            demuxerHandler->onMpt(*dynamic_cast<Mpt*>(table.get()));
            break;
        case MmtTableId::Plt:
            demuxerHandler->onPlt(*dynamic_cast<Plt*>(table.get()));
            break;
        case MmtTableId::MhBit:
            demuxerHandler->onMhBit(*dynamic_cast<MhBit*>(table.get()));
            break;
        case MmtTableId::MhAit:
            demuxerHandler->onMhAit(*dynamic_cast<MhAit*>(table.get()));
            break;
        case MmtTableId::Ddmt:
            demuxerHandler->onDdmt(*dynamic_cast<Ddmt*>(table.get()));
            break;
        case MmtTableId::Damt:
            demuxerHandler->onDamt(*dynamic_cast<Damt*>(table.get()));
            break;
        case MmtTableId::Dcct:
            demuxerHandler->onDcct(*dynamic_cast<Dcct*>(table.get()));
            break;
        }
    }
}

void MmtTlvDemuxer::processMmtTableStatistics(uint8_t tableId) {
    switch (tableId) {
    case MmtTableId::Pat:
        statistics.getMmtStat(mmtp.packetId)->setName("PAT");
        break;
    case MmtTableId::Ecm_0:
        statistics.getMmtStat(mmtp.packetId)->setName("ECM");
        break;
    case MmtTableId::Ecm_1:
        statistics.getMmtStat(mmtp.packetId)->setName("ECM");
        break;
    case MmtTableId::MhCdt:
        statistics.getMmtStat(mmtp.packetId)->setName("MH-CDT");
        break;
    case MmtTableId::MhEitPf:
        statistics.getMmtStat(mmtp.packetId)->setName("MH-EIT");
        break;
    case MmtTableId::MhEitS_0:
    case MmtTableId::MhEitS_1:
    case MmtTableId::MhEitS_2:
    case MmtTableId::MhEitS_3:
    case MmtTableId::MhEitS_4:
    case MmtTableId::MhEitS_5:
    case MmtTableId::MhEitS_6:
    case MmtTableId::MhEitS_7:
    case MmtTableId::MhEitS_8:
    case MmtTableId::MhEitS_9:
    case MmtTableId::MhEitS_10:
    case MmtTableId::MhEitS_11:
    case MmtTableId::MhEitS_12:
    case MmtTableId::MhEitS_13:
    case MmtTableId::MhEitS_14:
    case MmtTableId::MhEitS_15:
        statistics.getMmtStat(mmtp.packetId)->setName("MH-EIT");
        break;
    case MmtTableId::MhSdtActual:
        statistics.getMmtStat(mmtp.packetId)->setName("MH-SDT");
        break;
    case MmtTableId::MhSdtOther:
        statistics.getMmtStat(mmtp.packetId)->setName("MH-SDT");
        break;
    case MmtTableId::MhTot:
        statistics.getMmtStat(mmtp.packetId)->setName("MH-TOT");
        break;
    case MmtTableId::Mpt:
        statistics.getMmtStat(mmtp.packetId)->setName("MPT");
        break;
    case MmtTableId::Plt:
        statistics.getMmtStat(mmtp.packetId)->setName("PLT");
        break;
    case MmtTableId::MhBit:
        statistics.getMmtStat(mmtp.packetId)->setName("MH-BIT");
        break;
    case MmtTableId::Lct:
        statistics.getMmtStat(mmtp.packetId)->setName("LCT");
        break;
    case MmtTableId::Emm_0:
        statistics.getMmtStat(mmtp.packetId)->setName("EMM");
        break;
    case MmtTableId::Emm_1:
        statistics.getMmtStat(mmtp.packetId)->setName("EMM");
        break;
    case MmtTableId::Cat:
        statistics.getMmtStat(mmtp.packetId)->setName("CAT");
        break;
    case MmtTableId::Dcm:
        statistics.getMmtStat(mmtp.packetId)->setName("DCM");
        break;
    case MmtTableId::Dmm:
        statistics.getMmtStat(mmtp.packetId)->setName("DMM");
        break;
    case MmtTableId::MhSdtt:
        statistics.getMmtStat(mmtp.packetId)->setName("MH-SDTT");
        break;
    case MmtTableId::MhAit:
        statistics.getMmtStat(mmtp.packetId)->setName("MH-AIT");
        break;
    case MmtTableId::Ddmt:
        statistics.getMmtStat(mmtp.packetId)->setName("DDMT");
        break;
    case MmtTableId::Damt:
        statistics.getMmtStat(mmtp.packetId)->setName("DAMT");
        break;
    case MmtTableId::Dcct:
        statistics.getMmtStat(mmtp.packetId)->setName("DCCT");
        break;
    case MmtTableId::Emt:
        statistics.getMmtStat(mmtp.packetId)->setName("EMT");
        break;
    }
}

void MmtTlvDemuxer::processMmtPackageTable(const Mpt& mpt) {
    // Remove streams that do not exist in the MPT
    std::map<uint16_t, uint32_t> mapMpt; // packetId, assetType
    for (auto& asset : mpt.assets) {
        for (auto& locationInfo : asset.locationInfos) {
            if (locationInfo.locationType == 0) {
                mapMpt[locationInfo.packetId] = asset.assetType;
            }
        }
    }

    if (mapMpt.size()) {
        for (auto it = mapStream.begin(); it != mapStream.end(); ) {
            auto mptIt = mapMpt.find(it->first);
            if (mptIt != mapMpt.end()) {
                if (mptIt->second != it->second.assetType) {
                    mapFragmentValidator.erase(mptIt->first);
                    mapAssembler.erase(mptIt->first);
                    it = mapStream.erase(it);
                }
                else {
                    ++it;
                }
            }
            else {
                mapFragmentValidator.erase(it->first);
                mapAssembler.erase(it->first);
                it = mapStream.erase(it);
            }
        }
    }

    mapPacketIdByIdx.clear();

    int streamIndex = 0;
    for (const auto& asset : mpt.assets) {
        MmtStream* mmtStream = nullptr;

        for (const auto& locationInfo : asset.locationInfos) {
            if (locationInfo.locationType == 0) {
                if (asset.assetType == AssetType::hev1 ||
                    asset.assetType == AssetType::mp4a ||
                    asset.assetType == AssetType::stpp ||
                    asset.assetType == AssetType::aapp) {
                    auto [it, inserted] = mapStream.try_emplace(
                        locationInfo.packetId,
                        locationInfo.packetId
                    );
                    mmtStream = &it->second;
                    mmtStream->assetType = asset.assetType;
                    mmtStream->streamIndex = streamIndex;

                    if (!mmtStream->mpuProcessor) {
                        mmtStream->mpuProcessor = MpuProcessorFactory::create(mmtStream->assetType);
                    }

                    mapPacketIdByIdx[streamIndex] = locationInfo.packetId;
                    statistics.getMmtStat(locationInfo.packetId)->assetType = asset.assetType;
                    ++streamIndex;
                }
            }
        }

        if (!mmtStream) {
            continue;
        }

        if (asset.assetType == AssetType::stpp) {
            mmtStream->subtitleInfo.reset();
        }

        for (const auto& descriptor : asset.descriptors.list) {
            switch (descriptor->getDescriptorTag()) {
            case MpuTimestampDescriptor::kDescriptorTag:
            {
                processMpuTimestampDescriptor(*static_cast<const MpuTimestampDescriptor*>(descriptor.get()), *mmtStream);
                break;
            }
            case MpuExtendedTimestampDescriptor::kDescriptorTag:
            {
                processMpuExtendedTimestampDescriptor(*static_cast<const MpuExtendedTimestampDescriptor*>(descriptor.get()), *mmtStream);
                break;
            }
            case MhStreamIdentificationDescriptor::kDescriptorTag:
            {
                const auto* mmtDescriptor = static_cast<const MhStreamIdentificationDescriptor*>(descriptor.get());
                mmtStream->componentTag = mmtDescriptor->componentTag;
                break;
            }
            case VideoComponentDescriptor::kDescriptorTag:
            {
                const auto* mmtDescriptor = static_cast<const VideoComponentDescriptor*>(descriptor.get());
                mmtStream->videoComponentDescriptor = *mmtDescriptor;

                statistics.getMmtStat(mmtStream->packetId)->videoResolution = mmtDescriptor->videoResolution;
                statistics.getMmtStat(mmtStream->packetId)->videoAspectRatio = mmtDescriptor->videoAspectRatio;
                break;
            }
            case MhAudioComponentDescriptor::kDescriptorTag:
            {
                const auto* mmtDescriptor = static_cast<const MhAudioComponentDescriptor*>(descriptor.get());
                mmtStream->mhAudioComponentDescriptor = *mmtDescriptor;
                
                statistics.getMmtStat(mmtStream->packetId)->audioComponentType = mmtDescriptor->componentType;
                statistics.getMmtStat(mmtStream->packetId)->audioSamplingRate = mmtDescriptor->samplingRate;
                break;
            }
            case MhDataComponentDescriptor::kDescriptorTag:
            {
                const auto* mmtDescriptor = static_cast<const MhDataComponentDescriptor*>(descriptor.get());
                if (asset.assetType == AssetType::stpp && mmtDescriptor->dataComponentId == 0x0020) {
                    // Only publish a fully parsed descriptor. A truncated one
                    // leaves the struct half-filled, and the subtitle path
                    // treats an engaged subtitleInfo as authoritative - it
                    // would signal ARIB-TTML with a zero reference time and
                    // copy the language code straight into the output stream.
                    AdditionalAribSubtitleInfo subtitleInfo;
                    Common::ReadStream stream(mmtDescriptor->additionalDataComponentInfo);
                    if (subtitleInfo.unpack(stream)) {
                        mmtStream->subtitleInfo = std::move(subtitleInfo);
                    }
                }
                break;
            }
            }
        }
    }
}

void MmtTlvDemuxer::processMpuTimestampDescriptor(const MpuTimestampDescriptor& descriptor, MmtStream& mmtStream) {
    for (const auto& ts : descriptor.entries) {
        bool find = false;
        for (size_t i = 0; i < mmtStream.mpuTimestamps.size(); i++) {
            if (mmtStream.mpuTimestamps[i].mpuSequenceNumber == ts.mpuSequenceNumber) {
                mmtStream.mpuTimestamps[i].mpuPresentationTime = ts.mpuPresentationTime;
                find = true;
                break;
            }
        }
        if (find) {
            continue;
        }

        for (size_t i = 0; i < mmtStream.mpuTimestamps.size(); i++) {
            if (mmtStream.mpuTimestamps[i].mpuSequenceNumber < mmtStream.lastMpuSequenceNumber) {
                mmtStream.mpuTimestamps[i].mpuSequenceNumber = ts.mpuSequenceNumber;
                mmtStream.mpuTimestamps[i].mpuPresentationTime = ts.mpuPresentationTime;
                find = true;
                break;
            }
        }

        if (find) {
            continue;
        }

        if (mmtStream.mpuTimestamps.size() >= 100) {
            auto minElement = std::min_element(mmtStream.mpuTimestamps.begin(), mmtStream.mpuTimestamps.end(), 
                [](const auto& lhs, const auto& rhs) {
                    return lhs.mpuSequenceNumber < rhs.mpuSequenceNumber;
                });

            if (minElement != mmtStream.mpuTimestamps.end()) {
                minElement->mpuSequenceNumber = ts.mpuSequenceNumber;
                minElement->mpuPresentationTime = ts.mpuPresentationTime;
            }
        }
        else {
            mmtStream.mpuTimestamps.push_back(ts);
        }
    }
}

void MmtTlvDemuxer::processMpuExtendedTimestampDescriptor(const MpuExtendedTimestampDescriptor& descriptor, MmtStream& mmtStream) {
    if (descriptor.timescaleFlag) {
        mmtStream.timeBase.num = 1;
        mmtStream.timeBase.den = descriptor.timescale;
    }

    auto updateTimestamp = [](auto& target, const auto& source) {
        target.mpuSequenceNumber = source.mpuSequenceNumber;
        target.mpuDecodingTimeOffset = source.mpuDecodingTimeOffset;
        target.numOfAu = source.numOfAu;
        target.ptsOffsets = source.ptsOffsets;
        target.dtsPtsOffsets = source.dtsPtsOffsets;
    };

    for (const auto& ts : descriptor.entries) {
        if (mmtStream.lastMpuSequenceNumber > ts.mpuSequenceNumber)
            continue;

        bool find = false;
        for (size_t i = 0; i < mmtStream.mpuExtendedTimestamps.size(); i++) {
            if (mmtStream.mpuExtendedTimestamps[i].mpuSequenceNumber == ts.mpuSequenceNumber) {
                updateTimestamp(mmtStream.mpuExtendedTimestamps[i], ts);
                find = true;
                break;
            }
        }
        if (find) {
            continue;
        }

        for (size_t i = 0; i < mmtStream.mpuExtendedTimestamps.size(); i++) {
            if (mmtStream.mpuExtendedTimestamps[i].mpuSequenceNumber < mmtStream.lastMpuSequenceNumber) {
                updateTimestamp(mmtStream.mpuExtendedTimestamps[i], ts);
                find = true;
                break;
            }
        }
        if (find) {
            continue;
        }

        if (mmtStream.mpuExtendedTimestamps.size() >= 100) {
            auto minElement = std::min_element(mmtStream.mpuExtendedTimestamps.begin(), mmtStream.mpuExtendedTimestamps.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.mpuSequenceNumber < rhs.mpuSequenceNumber;
                });

            if (minElement != mmtStream.mpuExtendedTimestamps.end()) {
                updateTimestamp(*minElement, ts);
            }
        }
        else {
            mmtStream.mpuExtendedTimestamps.push_back(ts);
        }
    }
}

void MmtTlvDemuxer::processEcm(const Ecm& ecm) {
    if (!casHandler) {
        return;
    }

    casHandler->onEcm(ecm.ecmData);
}

void MmtTlvDemuxer::clear() {
    mapAssembler.clear();
    mapFragmentValidator.clear();
    mfuData.clear();
    mapStream.clear();
    mapPacketIdByIdx.clear();
    statistics.clear();

    if (casHandler) {
        casHandler->clear();
    }
}

void MmtTlvDemuxer::resetStreams() {
    mapAssembler.clear();
    mapFragmentValidator.clear();
    mfuData.clear();
    // Keep mapStream and mapPacketIdByIdx so stream registration survives the seek.
    // Reset per-stream sequence state so the demuxer accepts any starting sequence number.
    for (auto& [id, stream] : mapStream) {
        stream.lastMpuSequenceNumber.reset();
        stream.auIndex = 0;
        stream.mpuTimestamps.clear();
        stream.mpuExtendedTimestamps.clear();
        if (stream.mpuProcessor)
            stream.mpuProcessor->clear();
    }
}

void MmtTlvDemuxer::printStatistics() const {
    statistics.print();
}

FragmentAssembler* MmtTlvDemuxer::getAssembler(uint16_t packetId) {
    auto it = mapAssembler.find(packetId);
    if (it != mapAssembler.end()) {
        return it->second.get();
    }

    auto validator = std::make_unique<FragmentAssembler>();
    auto ptr = validator.get();
    mapAssembler.emplace(packetId, std::move(validator));
    return ptr;
}

FragmentValidator* MmtTlvDemuxer::getFragmentValidator(uint16_t packetId) {
    auto it = mapFragmentValidator.find(packetId);
    if (it != mapFragmentValidator.end()) {
        return it->second.get();
    }

    auto validator = std::make_unique<FragmentValidator>();
    auto ptr = validator.get();
    mapFragmentValidator.emplace(packetId, std::move(validator));
    return ptr;
}

MmtStream* MmtTlvDemuxer::getStream(uint16_t packetId) {
    auto it = mapStream.find(packetId);
    if (it == mapStream.end()) {
        return nullptr;
    }
    else {
        return &(it->second);
    }
}

MmtStream* MmtTlvDemuxer::getStreamByIdx(uint16_t idx) {
    auto it = mapPacketIdByIdx.find(idx);
    if (it == mapPacketIdByIdx.end()) {
        return nullptr;
    }
    else {
        return getStream(it->second);
    }
}

void MmtTlvDemuxer::processMpu(Common::ReadStream& stream) {
    if (!mpu.unpack(stream)) {
        return;
    }

    MmtStream* mmtStream = getStream(mmtp.packetId);
    if (!mmtStream) {
        return;
    }

    if (mpu.aggregateFlag && mpu.fragmentationIndicator != FragmentationIndicator::NotFragmented) {
        return;
    }

    if (mpu.fragmentType != FragmentType::Mfu) {
        return;
    }

    if (!mmtStream->lastMpuSequenceNumber) {
        mmtStream->lastMpuSequenceNumber = mpu.mpuSequenceNumber;
    }

    if (mpu.mpuSequenceNumber == *mmtStream->lastMpuSequenceNumber + 1) {
        mmtStream->lastMpuSequenceNumber = mpu.mpuSequenceNumber;
        mmtStream->auIndex = 0;
    }
    else if (mpu.mpuSequenceNumber != *mmtStream->lastMpuSequenceNumber) {
        // MPU sequence discontinuity (packet loss spanning MPUs, or a seam in a
        // stream stitched from RAP-aligned cuts). Without a resync the sequence
        // tracker can never advance again (+1 chain is broken) and timestamp
        // lookups fail for every following MPU, muting the stream for good.
        const bool backward = mpu.mpuSequenceNumber < *mmtStream->lastMpuSequenceNumber;
        mmtStream->lastMpuSequenceNumber = mpu.mpuSequenceNumber;
        mmtStream->auIndex = 0;
        if (mmtStream->mpuProcessor) {
            mmtStream->mpuProcessor->clear();
        }
        if (backward) {
            // Stale entries with larger sequence numbers would block the
            // replacement rules in processMpuTimestampDescriptor.
            mmtStream->mpuTimestamps.clear();
            mmtStream->mpuExtendedTimestamps.clear();
        }
    }

    auto validator = getFragmentValidator(mmtp.packetId);
    auto assembler = getAssembler(mmtp.packetId);

    if (mpu.mpuSequenceNumber != mmtStream->lastMpuSequenceNumber) {
        if (mpu.fragmentationIndicator == FragmentationIndicator::NotFragmented) {
            validator->clear();
            assembler->clear();
        }
    }

    mmtStream->rapFlag = mmtp.rapFlag;

    Common::ReadStream nstream(mpu.payload);
    if (mpu.aggregateFlag == 0) {
        if (!dataUnit.unpack(nstream, mpu.timedFlag, mpu.aggregateFlag)) {
            return;
        }

        if (mmtStream->assetType == AssetType::hev1 ||
            mmtStream->assetType == AssetType::mp4a ||
            mmtStream->assetType == AssetType::aapp) {
            if (validator->validate(mpu.fragmentationIndicator, mmtp.packetSequenceNumber)) {
                Common::ReadStream dataStream(dataUnit.data);
                processMfuData(dataStream);
            }
        }
        else {
            if (assembler->assemble(dataUnit.data, mpu.fragmentationIndicator, mmtp.packetSequenceNumber)) {
                Common::ReadStream dataStream(assembler->data);
                processMfuData(dataStream);
                assembler->clear();
            }
        }
    }
    else {
        while (!nstream.isEof()) {
            if (!dataUnit.unpack(nstream, mpu.timedFlag, mpu.aggregateFlag)) {
                return;
            }

            if (mmtStream->assetType == AssetType::hev1 ||
                mmtStream->assetType == AssetType::mp4a) {
                if (validator->validate(mpu.fragmentationIndicator, mmtp.packetSequenceNumber)) {
                    Common::ReadStream dataStream(dataUnit.data);
                    processMfuData(dataStream);
                }
            }
            else {
                if (assembler->assemble(dataUnit.data, mpu.fragmentationIndicator, mmtp.packetSequenceNumber)) {
                    Common::ReadStream dataStream(assembler->data);
                    processMfuData(dataStream);
                    assembler->clear();
                }
            }
        }
    }
}

void MmtTlvDemuxer::processMfuData(Common::ReadStream& stream) {
    MmtStream* mmtStream = getStream(mmtp.packetId);
    auto validator = getFragmentValidator(mmtp.packetId);

    if (!mmtStream || !mmtStream->mpuProcessor) {
        return;
    }

    std::vector<uint8_t> data(stream.leftBytes());
    stream.read(data.data(), stream.leftBytes());

    const auto ret = mmtStream->mpuProcessor->process(*mmtStream, data, mpu.fragmentationIndicator);
    // A processor returns Dropped when it discards real data (parse error, size
    // cap, missing timestamp). Count that as an output drop regardless of the
    // fragmentation indicator - the previous code only counted it on the
    // Not/Last fragment, so discarded First/Middle fragments (e.g. an oversized
    // video NAL) went silently unreported.
    if (ret.status == MfuProcessStatus::Dropped) {
        statistics.getMmtStat(mmtp.packetId)->outputDrop++;
        validator->clear();
        return;
    }
    if (ret.status != MfuProcessStatus::Output || !ret.data) {
        return; // Accumulating: input consumed, nothing to output yet.
    }

    const auto& mfuData = *ret.data;
    auto* outStream = getStreamByIdx(mfuData.streamIndex);
    if (!outStream) {
        return;
    }

    if (demuxerHandler) {
        switch (outStream->assetType) {
        case AssetType::hev1:
            demuxerHandler->onVideoData(*outStream, mfuData);
            break;
        case AssetType::mp4a:
            demuxerHandler->onAudioData(*outStream, mfuData);
            break;
        case AssetType::stpp:
            demuxerHandler->onSubtitleData(*outStream, mfuData);
            break;
        case AssetType::aapp:
            demuxerHandler->onApplicationData(*outStream, mpu, dataUnit, mfuData);
            break;
        }
    }
}

void MmtTlvDemuxer::processSignalingMessages(Common::ReadStream& stream) {
    SignalingMessage signalingMessage;
    if (!signalingMessage.unpack(stream)) {
        return;
    }

    auto assembler = getAssembler(mmtp.packetId);
    assembler->checkState(mmtp.packetSequenceNumber);

    if (!signalingMessage.aggregationFlag) {
        if (assembler->assemble(signalingMessage.payload, signalingMessage.fragmentationIndicator, mmtp.packetSequenceNumber)) {
            Common::ReadStream messageStream(assembler->data);
            processSignalingMessage(messageStream);
            assembler->clear();
        }
    }
    else {
        if (signalingMessage.fragmentationIndicator != FragmentationIndicator::NotFragmented) {
            return;
        }

        Common::ReadStream nstream(signalingMessage.payload);
        while (!nstream.isEof()) {
            uint32_t length;
            if (signalingMessage.lengthExtensionFlag)
                length = nstream.getBe32U();
            else
                length = nstream.getBe16U();

            if (length > nstream.leftBytes()) {
                return;
            }
            std::vector<uint8_t> message;
            message.resize(length);
            nstream.read(message.data(), length);

            if (assembler->assemble(message, signalingMessage.fragmentationIndicator, mmtp.packetSequenceNumber)) {
                Common::ReadStream messageStream(assembler->data);
                processSignalingMessage(messageStream);
                assembler->clear();
            }
        }
    }
}

void MmtTlvDemuxer::processSignalingMessage(Common::ReadStream& stream) {
    MmtMessageId id = static_cast<MmtMessageId>(stream.peekBe16U());

    switch (id) {
    case MmtMessageId::PaMessage:
        return processPaMessage(stream);
    case MmtMessageId::M2SectionMessage:
        return processM2SectionMessage(stream);
    case MmtMessageId::CaMessage:
        return processCaMessage(stream);
    case MmtMessageId::M2ShortSectionMessage:
        return processM2ShortSectionMessage(stream);
    case MmtMessageId::DataTransmissionMessage:
        return processDataTransmissionMessage(stream);
    }
}

bool MmtTlvDemuxer::isValidTlv(Common::ReadStream& stream) const {
    if (stream.leftBytes() < 2) {
        return false;
    }

    uint8_t bytes[2];
    stream.peek(bytes, 2);

    // syncByte
    if (bytes[0] != 0x7F) {
        return false;
    }

    // packetType
    if (bytes[1] > 0x04 && bytes[1] < 0xFD) {
        return false;
    }

    return true;
}

}
