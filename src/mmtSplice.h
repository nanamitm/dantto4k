#pragma once
#include <cstdint>
#include <optional>
#include <vector>

// Splice primitives for rewriting decrypted MMT/TLV (.mmts) streams.
//
// Used by frame-accurate .mmts editing: most of the stream is copied untouched,
// but the partial GOPs at cut boundaries are re-encoded and re-packed as
// synthesized video MPUs, and the MPT's mpu_timestamp / mpu_extended_timestamp
// descriptor entries for those MPUs are rewritten so demuxers can resolve
// their timestamps. This is deliberately NOT a full MMT muxer: everything that
// is not a video MPU or an MPT timestamp entry passes through byte-identical.
namespace MmtTlv {

namespace Splice {

// --------------------------------------------------------------------------
// Lightweight TLV packet parser (just deep enough for splice decisions).
// --------------------------------------------------------------------------
struct PacketInfo {
    size_t size = 0;       // whole TLV packet size in bytes
    uint8_t tlvType = 0;   // TlvPacketType

    bool mmtp = false;     // headers below valid only when true
    uint16_t contextId = 0;
    uint16_t packetId = 0;
    uint8_t payloadType = 0; // PayloadType raw value
    uint32_t packetSequenceNumber = 0;
    uint32_t deliveryTimestamp = 0;
    bool rapFlag = false;

    bool mpu = false;      // payloadType == Mpu and MPU header parsed
    uint32_t mpuSequenceNumber = 0;
    uint8_t mpuFragmentType = 0; // FragmentType raw value

    bool signaling = false; // payloadType == ContainsOneOrMoreControlMessage
    uint8_t signalingFragmentationIndicator = 0;
    bool signalingAggregation = false;
    bool signalingLengthExtension = false;
    size_t signalingPayloadOffset = 0; // offset of SignalingMessage payload in packet
};

// Parses the TLV packet at data[0..avail). Returns false if there is no full,
// valid packet at the position (caller should resync / read more).
bool parsePacket(const uint8_t* data, size_t avail, PacketInfo& out);

// --------------------------------------------------------------------------
// Video MPU packing: HEVC access units -> MFU/MMTP/TLV packet bytes.
// --------------------------------------------------------------------------
struct VideoAu {
    std::vector<uint8_t> annexb; // one access unit (Annex-B start codes)
    bool keyframe = false;
};

struct VideoMpuParams {
    uint16_t packetId = 0;
    uint16_t contextId = 0;
    uint32_t deliveryTimestamp = 0;
    uint32_t mpuSequenceNumber = 0;
    uint32_t firstPacketSequenceNumber = 0;
    size_t maxFragmentPayload = 60000; // NAL bytes per MFU fragment packet
};

// Packs the AUs as a single timed MPU (one NAL per MFU; large NALs fragmented).
// An AUD NAL is synthesized in front of any AU that does not start with one,
// since downstream demuxers assign timestamps on the AUD. The first packet
// carries the MMTP RAP flag. Returns complete TLV packets in emit order.
std::vector<std::vector<uint8_t>> packVideoMpu(const std::vector<VideoAu>& aus,
                                               const VideoMpuParams& params);

// Number of TLV packets packVideoMpu() will produce for these AUs (lets the
// caller pre-compute firstPacketSequenceNumber so the packed run ends or
// starts adjacent to copied packets).
size_t countVideoMpuPackets(const std::vector<VideoAu>& aus,
                            size_t maxFragmentPayload = 60000);

// --------------------------------------------------------------------------
// MPT timestamp patching.
// --------------------------------------------------------------------------

// Timing of one synthesized MPU, in the asset's timescale ticks (offsets) and
// absolute microseconds (presentation time; same epoch the source stream
// uses in its mpu_timestamp_descriptor NTP values).
struct MpuTiming {
    uint32_t mpuSequenceNumber = 0;
    uint64_t presentationTimeUs = 0;   // first presented AU
    uint16_t mpuDecodingTimeOffset = 0;
    std::vector<uint16_t> ptsOffsets;    // per AU: dts(i+1)-dts(i) (last = duration)
    std::vector<uint16_t> dtsPtsOffsets; // per AU: pts(i)-dts(i)
};

// Rebuilds `mptTable` (raw MPT table bytes, starting at table_id) so that the
// asset whose location carries `videoPacketId`:
//  - loses all mpu_timestamp / mpu_extended_timestamp entries with sequence
//    numbers in [dropSeqMin, dropSeqMax] (the replaced original MPUs), and
//  - gains entries for `newMpus`.
// Other assets and descriptors are byte-identical. Returns std::nullopt when
// the table cannot be parsed or the asset is not found.
std::optional<std::vector<uint8_t>> patchMptVideoTimestamps(
    const std::vector<uint8_t>& mptTable, uint16_t videoPacketId,
    const std::vector<MpuTiming>& newMpus,
    uint32_t dropSeqMin, uint32_t dropSeqMax);

// mpu_timestamp_descriptor entries of every asset in the MPT, as
// (packetId, mpuSequenceNumber, presentationTimeUs). Used to time-gate
// audio/subtitle MPUs at cut boundaries.
struct PidMpuTime {
    uint16_t packetId = 0;
    uint32_t mpuSequenceNumber = 0;
    uint64_t presentationTimeUs = 0;
};
std::vector<PidMpuTime> collectMpuTimestamps(const std::vector<uint8_t>& mptTable);

// Timescale of the asset carrying `videoPacketId` (from its
// mpu_extended_timestamp_descriptor), or 0 when not present.
uint32_t videoTimescale(const std::vector<uint8_t>& mptTable, uint16_t videoPacketId);

// Wraps a (patched) MPT table into one unfragmented PA-message signaling TLV
// packet on `packetId` (the pid the original MPT was carried on).
std::vector<uint8_t> packPaMptPacket(const std::vector<uint8_t>& mptTable,
                                     uint16_t packetId,
                                     uint16_t contextId,
                                     uint32_t packetSequenceNumber,
                                     uint32_t deliveryTimestamp,
                                     uint8_t paVersion);

// Absolute microseconds <-> the NTP64 value used by mpu_timestamp_descriptor.
uint64_t usToNtp(uint64_t us);

} // namespace Splice

} // namespace MmtTlv
