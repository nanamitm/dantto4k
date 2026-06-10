#include "mmtSplice.h"

#include "mmtFragment.h"
#include "tlv.h"

#include <cstring>

namespace MmtTlv {

namespace Splice {

namespace {

constexpr uint64_t kNtpOffsetUs = 2208988800ULL * 1000000ULL;

constexpr uint16_t kMpuTimestampTag = 0x0001;
constexpr uint16_t kMpuExtendedTimestampTag = 0x8026;

inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline uint64_t be64(const uint8_t* p)
{
    return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4);
}

inline void putBe16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
inline void putBe32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
inline void putBe64(std::vector<uint8_t>& v, uint64_t x)
{
    putBe32(v, static_cast<uint32_t>(x >> 32));
    putBe32(v, static_cast<uint32_t>(x));
}

uint64_t ntpToUs(uint64_t ntp)
{
    const uint64_t sec = ntp >> 32;
    const uint64_t frac = ntp & 0xFFFFFFFFULL;
    const uint64_t us = sec * 1000000ULL + (frac * 1000000ULL) / 0xFFFFFFFFULL;
    return us >= kNtpOffsetUs ? us - kNtpOffsetUs : 0;
}

// ---------------------------------------------------------------------------
// MPT walker: locates each asset's descriptor area without fully decoding it.
// ---------------------------------------------------------------------------
struct AssetView {
    uint16_t packetId = 0;      // first locationType==0 packetId (0 if none)
    size_t descLenOffset = 0;   // offset of the 2-byte assetDescriptorsLength
    size_t descOffset = 0;      // offset of the descriptor bytes
    size_t descLength = 0;
};

struct MptView {
    size_t tableLengthOffset = 0; // 2-byte length right after tableId+version
    std::vector<AssetView> assets;
};

size_t locationInfoSize(const uint8_t* p, size_t avail)
{
    if (avail < 1)
        return 0;
    switch (p[0]) {
    case 0: return 1 + 2;
    case 1: return 1 + 4 + 4 + 2 + 2;
    case 2: return 1 + 16 + 16 + 2 + 2;
    case 3: return 1 + 2 + 2 + 2;
    case 4: return 1 + 16 + 16 + 2 + 2;
    case 5: return avail >= 2 ? 2 + static_cast<size_t>(p[1]) : 0;
    default: return 0;
    }
}

bool parseMpt(const std::vector<uint8_t>& t, MptView& out)
{
    size_t pos = 0;
    auto need = [&](size_t n) { return pos + n <= t.size(); };

    if (!need(2) || t[0] != 0x20) // MmtTableId::Mpt
        return false;
    pos = 2;                      // tableId, version
    out.tableLengthOffset = pos;
    if (!need(2)) return false;
    pos += 2;                     // length
    if (!need(1)) return false;
    pos += 1;                     // reserved/mptMode
    if (!need(1)) return false;
    const size_t pkgIdLen = t[pos];
    pos += 1;
    if (!need(pkgIdLen)) return false;
    pos += pkgIdLen;
    if (!need(2)) return false;
    const size_t mptDescLen = be16(&t[pos]);
    pos += 2;
    if (!need(mptDescLen)) return false;
    pos += mptDescLen;
    if (!need(1)) return false;
    const size_t numAssets = t[pos];
    pos += 1;

    for (size_t i = 0; i < numAssets; i++) {
        AssetView a;
        if (!need(1 + 4 + 1)) return false;
        pos += 1 + 4;             // identifierType, assetIdScheme
        const size_t assetIdLen = t[pos];
        pos += 1;
        if (!need(assetIdLen)) return false;
        pos += assetIdLen;
        if (!need(4 + 1 + 1)) return false;
        pos += 4 + 1;             // assetType, flags
        const size_t locCount = t[pos];
        pos += 1;
        for (size_t l = 0; l < locCount; l++) {
            const size_t sz = locationInfoSize(&t[pos], t.size() - pos);
            if (sz == 0 || !need(sz)) return false;
            if (t[pos] == 0 && a.packetId == 0)
                a.packetId = be16(&t[pos + 1]);
            pos += sz;
        }
        if (!need(2)) return false;
        a.descLenOffset = pos;
        a.descLength = be16(&t[pos]);
        pos += 2;
        a.descOffset = pos;
        if (!need(a.descLength)) return false;
        pos += a.descLength;
        out.assets.push_back(a);
    }
    return true;
}

// Walks the descriptors in [desc, desc+len): calls fn(tag, full descriptor
// bytes incl. header, payload offset). Returns false on a malformed area.
template <typename Fn>
bool walkDescriptors(const uint8_t* desc, size_t len, Fn&& fn)
{
    size_t pos = 0;
    while (pos + 3 <= len) {
        const uint16_t tag = be16(desc + pos);
        size_t hdr, dlen;
        if (tag == 0xF001 || tag == 0xF002) { // 16-bit-length descriptors
            if (pos + 4 > len) return false;
            hdr = 4;
            dlen = be16(desc + pos + 2);
        } else {
            hdr = 3;
            dlen = desc[pos + 2];
        }
        if (pos + hdr + dlen > len)
            return false;
        fn(tag, desc + pos, hdr + dlen, hdr);
        pos += hdr + dlen;
    }
    return pos == len;
}

// mpu_timestamp_descriptor instances for raw 12-byte entries (self-limited by
// the descriptor length, so multiple instances and any position are fine).
void appendBasicTimestampDescriptors(std::vector<uint8_t>& out,
                                     const std::vector<uint8_t>& rawEntries)
{
    const size_t total = rawEntries.size() / 12;
    for (size_t i = 0; i < total; i += 21) {
        const size_t n = std::min<size_t>(21, total - i);
        putBe16(out, kMpuTimestampTag);
        out.push_back(static_cast<uint8_t>(n * 12));
        out.insert(out.end(), rawEntries.begin() + i * 12, rawEntries.begin() + (i + n) * 12);
    }
}

// dantto4k's MpuExtendedTimestampDescriptor parser reads entries until the END
// of the asset's descriptor area (it ignores the descriptor length), which the
// broadcast layout satisfies by always placing one instance last. The patched
// asset must therefore carry exactly ONE 0x8026 instance, placed last, and all
// its entries (new + kept originals) must fit its 8-bit length (<= 255 bytes).
void appendExtendedTimestampDescriptor(std::vector<uint8_t>& out,
                                       const std::vector<MpuTiming>& entries,
                                       uint32_t timescale)
{
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>((2 << 1) | 1)); // ptsOffsetType=2, timescaleFlag=1
    putBe32(body, timescale);
    for (const auto& m : entries) {
        const size_t numAu = m.dtsPtsOffsets.size();
        const size_t entryLen = 4 + 1 + 2 + 1 + numAu * 4;
        if (m.ptsOffsets.size() != numAu || body.size() + entryLen > 255)
            continue; // keep what fits; later MPTs re-announce dropped originals
        putBe32(body, m.mpuSequenceNumber);
        body.push_back(0); // leap indicator / reserved
        putBe16(body, m.mpuDecodingTimeOffset);
        body.push_back(static_cast<uint8_t>(numAu));
        for (size_t a = 0; a < numAu; a++) {
            putBe16(body, m.dtsPtsOffsets[a]);
            putBe16(body, m.ptsOffsets[a]);
        }
    }
    putBe16(out, kMpuExtendedTimestampTag);
    out.push_back(static_cast<uint8_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

} // anonymous namespace

// ---------------------------------------------------------------------------
bool parsePacket(const uint8_t* data, size_t avail, PacketInfo& out)
{
    if (avail < 4 || data[0] != 0x7F)
        return false;
    const uint8_t type = data[1];
    const size_t len = be16(data + 2);
    if (4 + len > avail)
        return false;

    out = PacketInfo{};
    out.size = 4 + len;
    out.tlvType = type;
    if (type != static_cast<uint8_t>(TlvPacketType::HeaderCompressedIpPacket))
        return true;

    const uint8_t* p = data + 4;
    size_t left = len;
    if (left < 3)
        return true;
    out.contextId = static_cast<uint16_t>(be16(p) >> 4);
    const uint8_t headerType = p[2];
    size_t pos = 3;
    if (headerType == 0x60) { // partial IPv6 + partial UDP
        if (left < pos + 38 + 4)
            return true;
        pos += 38 + 4;
    } else if (headerType != 0x61 && headerType != 0x20 && headerType != 0x21) {
        return true; // unknown compression; treat as opaque
    }

    // MMTP fixed header
    if (left < pos + 12)
        return true;
    const uint8_t b0 = p[pos];
    const bool packetCounterFlag = (b0 >> 5) & 1;
    const bool extensionHeaderFlag = (b0 >> 1) & 1;
    out.rapFlag = b0 & 1;
    out.payloadType = p[pos + 1] & 0x3F;
    out.packetId = be16(p + pos + 2);
    out.deliveryTimestamp = be32(p + pos + 4);
    out.packetSequenceNumber = be32(p + pos + 8);
    pos += 12;
    if (packetCounterFlag) {
        if (left < pos + 4) return true;
        pos += 4;
    }
    if (extensionHeaderFlag) {
        if (left < pos + 4) return true;
        const size_t ehLen = be16(p + pos + 2);
        pos += 4;
        if (left < pos + ehLen) return true;
        pos += ehLen;
    }
    out.mmtp = true;

    if (out.payloadType == 0x00) { // Mpu
        if (left < pos + 8)
            return true;
        const size_t payloadLength = be16(p + pos);
        if (payloadLength != left - pos - 2)
            return true; // malformed; headers above still useful
        out.mpuFragmentType = p[pos + 2] >> 4;
        out.mpuSequenceNumber = be32(p + pos + 4);
        out.mpu = true;
    } else if (out.payloadType == 0x02) { // signaling
        if (left < pos + 2)
            return true;
        out.signalingFragmentationIndicator = (p[pos] >> 6) & 0b11;
        out.signalingLengthExtension = (p[pos] >> 1) & 1;
        out.signalingAggregation = p[pos] & 1;
        out.signalingPayloadOffset = 4 + pos + 2;
        out.signaling = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
namespace {

std::vector<std::pair<size_t, size_t>> splitAnnexB(const std::vector<uint8_t>& au)
{
    std::vector<std::pair<size_t, size_t>> nals; // (offset, size) of NAL bodies
    size_t pos = 0;
    size_t start = SIZE_MAX;
    while (pos + 3 <= au.size()) {
        if (au[pos] == 0 && au[pos + 1] == 0 &&
            (au[pos + 2] == 1 || (pos + 4 <= au.size() && au[pos + 2] == 0 && au[pos + 3] == 1))) {
            const size_t scLen = (au[pos + 2] == 1) ? 3 : 4;
            if (start != SIZE_MAX)
                nals.emplace_back(start, pos - start);
            pos += scLen;
            start = pos;
        } else {
            ++pos;
        }
    }
    if (start != SIZE_MAX && start < au.size())
        nals.emplace_back(start, au.size() - start);
    return nals;
}

struct MfuChunk {
    const uint8_t* data;       // nullptr -> synthesized AUD
    std::vector<uint8_t> head; // 4-byte NAL length prefix (+ AUD bytes)
    size_t bodyOffset = 0;     // into data
    size_t bodySize = 0;
    FragmentationIndicator fi = FragmentationIndicator::NotFragmented;
    uint32_t sampleNumber = 0;
};

// Enumerates the MFU fragments for the AUs: one NAL per MFU, each MFU payload
// is [4-byte length][NAL], large NALs split into First/Middle/Last fragments.
template <typename Emit>
void forEachMfuChunk(const std::vector<VideoAu>& aus, size_t maxFrag, Emit&& emit)
{
    static const uint8_t kAud[] = { 0x46, 0x01, 0x50 }; // access_unit_delimiter
    uint32_t sample = 1;
    for (const auto& au : aus) {
        const auto nals = splitAnnexB(au.annexb);
        bool needAud = true;
        if (!nals.empty()) {
            const uint8_t t = (au.annexb[nals[0].first] >> 1) & 0x3F;
            needAud = (t != 0x23);
        }
        if (needAud) {
            MfuChunk c;
            c.data = nullptr;
            putBe32(c.head, sizeof(kAud));
            c.head.insert(c.head.end(), kAud, kAud + sizeof(kAud));
            c.fi = FragmentationIndicator::NotFragmented;
            c.sampleNumber = sample;
            emit(c);
        }
        for (const auto& [off, size] : nals) {
            if (size == 0)
                continue;
            if (4 + size <= maxFrag) {
                MfuChunk c;
                c.data = au.annexb.data();
                putBe32(c.head, static_cast<uint32_t>(size));
                c.bodyOffset = off;
                c.bodySize = size;
                c.fi = FragmentationIndicator::NotFragmented;
                c.sampleNumber = sample;
                emit(c);
            } else {
                size_t done = 0;
                bool first = true;
                while (done < size) {
                    MfuChunk c;
                    c.data = au.annexb.data();
                    size_t budget = maxFrag;
                    if (first) {
                        putBe32(c.head, static_cast<uint32_t>(size));
                        budget -= 4;
                    }
                    c.bodyOffset = off + done;
                    c.bodySize = std::min(budget, size - done);
                    done += c.bodySize;
                    c.fi = first
                        ? FragmentationIndicator::FirstFragment
                        : (done == size ? FragmentationIndicator::LastFragment
                                        : FragmentationIndicator::MiddleFragment);
                    c.sampleNumber = sample;
                    first = false;
                    emit(c);
                }
            }
        }
        ++sample;
    }
}

} // anonymous namespace

size_t countVideoMpuPackets(const std::vector<VideoAu>& aus, size_t maxFragmentPayload)
{
    size_t n = 0;
    forEachMfuChunk(aus, maxFragmentPayload, [&](const MfuChunk&) { ++n; });
    return n;
}

std::vector<std::vector<uint8_t>> packVideoMpu(const std::vector<VideoAu>& aus,
                                               const VideoMpuParams& params)
{
    std::vector<std::vector<uint8_t>> packets;
    uint32_t seq = params.firstPacketSequenceNumber;
    uint8_t cipSeq = 0;
    bool firstPacket = true;

    forEachMfuChunk(aus, params.maxFragmentPayload, [&](const MfuChunk& c) {
        const size_t dataLen = c.head.size() + c.bodySize;
        const size_t duLen = 14 + dataLen;           // DataUnit header + payload
        const size_t mpuLen = 6 + duLen;             // MPU header fields + DataUnit
        const size_t mmtpLen = 12 + 2 + mpuLen;      // MMTP hdr + payloadLength + MPU
        const size_t cipLen = 3 + mmtpLen;

        std::vector<uint8_t> pkt;
        pkt.reserve(4 + cipLen);
        pkt.push_back(0x7F);
        pkt.push_back(static_cast<uint8_t>(TlvPacketType::HeaderCompressedIpPacket));
        putBe16(pkt, static_cast<uint16_t>(cipLen));
        // Compressed IP: contextId + 4-bit sequence, no uncompressed headers.
        putBe16(pkt, static_cast<uint16_t>((params.contextId << 4) | (cipSeq & 0x0F)));
        pkt.push_back(0x61);
        cipSeq = (cipSeq + 1) & 0x0F;
        // MMTP
        pkt.push_back(firstPacket ? 0x01 : 0x00); // RAP flag on the MPU start
        pkt.push_back(0x00);                      // payloadType = Mpu
        putBe16(pkt, params.packetId);
        putBe32(pkt, params.deliveryTimestamp);
        putBe32(pkt, seq++);
        // MPU
        putBe16(pkt, static_cast<uint16_t>(mpuLen));
        pkt.push_back(static_cast<uint8_t>(
            (static_cast<uint8_t>(FragmentType::Mfu) << 4) | (1 << 3) |
            (static_cast<uint8_t>(c.fi) << 1)));
        pkt.push_back(0); // fragmentCounter (informational)
        putBe32(pkt, params.mpuSequenceNumber);
        // DataUnit (timed, non-aggregated)
        putBe32(pkt, 0);              // movieFragmentSequenceNumber
        putBe32(pkt, c.sampleNumber); // sampleNumber
        putBe32(pkt, 0);              // offset
        pkt.push_back(0);             // priority
        pkt.push_back(0);             // dependencyCounter
        pkt.insert(pkt.end(), c.head.begin(), c.head.end());
        if (c.data && c.bodySize)
            pkt.insert(pkt.end(), c.data + c.bodyOffset, c.data + c.bodyOffset + c.bodySize);

        packets.push_back(std::move(pkt));
        firstPacket = false;
    });
    return packets;
}

// ---------------------------------------------------------------------------
std::optional<std::vector<uint8_t>> patchMptVideoTimestamps(
    const std::vector<uint8_t>& mptTable, uint16_t videoPacketId,
    const std::vector<MpuTiming>& newMpus,
    uint32_t dropSeqMin, uint32_t dropSeqMax)
{
    MptView view;
    if (!parseMpt(mptTable, view))
        return std::nullopt;

    const AssetView* video = nullptr;
    for (const auto& a : view.assets) {
        if (a.packetId == videoPacketId) {
            video = &a;
            break;
        }
    }
    if (!video)
        return std::nullopt;

    const uint8_t* desc = mptTable.data() + video->descOffset;

    // Walk the original area: other descriptors are copied verbatim; the two
    // timestamp descriptor kinds are collected, filtered and re-emitted at the
    // end (the 0x8026 instance MUST be the last descriptor; see above).
    std::vector<uint8_t> rebuilt;
    rebuilt.reserve(video->descLength + 512);
    std::vector<uint8_t> keptBasic;          // raw 12-byte entries
    std::vector<MpuTiming> mergedExtended = newMpus;
    uint32_t timescale = 0;

    const auto inDropRange = [&](uint32_t seq) { return seq >= dropSeqMin && seq <= dropSeqMax; };
    const bool ok = walkDescriptors(desc, video->descLength,
        [&](uint16_t tag, const uint8_t* d, size_t total, size_t hdr) {
        if (tag == kMpuTimestampTag) {
            for (size_t pos = hdr; pos + 12 <= total; pos += 12) {
                if (!inDropRange(be32(d + pos)))
                    keptBasic.insert(keptBasic.end(), d + pos, d + pos + 12);
            }
            return;
        }
        if (tag == kMpuExtendedTimestampTag) {
            // Header: flags(1) [timescale(4)] [defaultPtsOffset(2)], then entries.
            size_t pos = hdr;
            if (pos >= total) return;
            const uint8_t flags = d[pos];
            const uint8_t ptsOffsetType = (flags >> 1) & 0b11;
            uint16_t defaultPtsOffset = 0;
            pos += 1;
            if (flags & 1) {
                if (pos + 4 > total) return;
                if (timescale == 0)
                    timescale = be32(d + pos);
                pos += 4;
            }
            if (ptsOffsetType == 1) {
                if (pos + 2 > total) return;
                defaultPtsOffset = be16(d + pos);
                pos += 2;
            }
            // Decode entries (normalized to explicit per-AU offsets) so they
            // can be merged into the single rebuilt instance.
            while (pos + 8 <= total) {
                MpuTiming t;
                t.mpuSequenceNumber = be32(d + pos);
                t.mpuDecodingTimeOffset = be16(d + pos + 5);
                const uint8_t numAu = d[pos + 7];
                const size_t entryLen = 8 + static_cast<size_t>(numAu) * (ptsOffsetType == 2 ? 4 : 2);
                if (pos + entryLen > total)
                    break;
                size_t q = pos + 8;
                for (uint8_t a = 0; a < numAu; a++) {
                    t.dtsPtsOffsets.push_back(be16(d + q));
                    q += 2;
                    if (ptsOffsetType == 2) {
                        t.ptsOffsets.push_back(be16(d + q));
                        q += 2;
                    } else {
                        t.ptsOffsets.push_back(defaultPtsOffset);
                    }
                }
                // presentationTimeUs unused for the extended entry emission.
                if (!inDropRange(t.mpuSequenceNumber))
                    mergedExtended.push_back(std::move(t));
                pos += entryLen;
            }
            return;
        }
        rebuilt.insert(rebuilt.end(), d, d + total); // other descriptors verbatim
    });
    if (!ok)
        return std::nullopt;
    if (timescale == 0)
        timescale = 90000;

    // New basic entries + kept originals.
    std::vector<uint8_t> basicEntries;
    for (const auto& m : newMpus) {
        putBe32(basicEntries, m.mpuSequenceNumber);
        putBe64(basicEntries, usToNtp(m.presentationTimeUs));
    }
    basicEntries.insert(basicEntries.end(), keptBasic.begin(), keptBasic.end());
    appendBasicTimestampDescriptors(rebuilt, basicEntries);
    appendExtendedTimestampDescriptor(rebuilt, mergedExtended, timescale);
    if (rebuilt.size() > 0xFFFF)
        return std::nullopt;

    std::vector<uint8_t> out;
    out.reserve(mptTable.size() + rebuilt.size());
    out.insert(out.end(), mptTable.begin(), mptTable.begin() + video->descLenOffset);
    putBe16(out, static_cast<uint16_t>(rebuilt.size()));
    out.insert(out.end(), rebuilt.begin(), rebuilt.end());
    out.insert(out.end(), mptTable.begin() + video->descOffset + video->descLength, mptTable.end());

    // Fix the MPT length field (bytes after the field itself).
    const size_t newLen = out.size() - (view.tableLengthOffset + 2);
    if (newLen > 0xFFFF)
        return std::nullopt;
    out[view.tableLengthOffset] = static_cast<uint8_t>(newLen >> 8);
    out[view.tableLengthOffset + 1] = static_cast<uint8_t>(newLen);
    return out;
}

std::vector<PidMpuTime> collectMpuTimestamps(const std::vector<uint8_t>& mptTable)
{
    std::vector<PidMpuTime> out;
    MptView view;
    if (!parseMpt(mptTable, view))
        return out;
    for (const auto& a : view.assets) {
        walkDescriptors(mptTable.data() + a.descOffset, a.descLength,
            [&](uint16_t tag, const uint8_t* d, size_t total, size_t hdr) {
            if (tag != kMpuTimestampTag)
                return;
            for (size_t pos = hdr; pos + 12 <= total; pos += 12) {
                PidMpuTime e;
                e.packetId = a.packetId;
                e.mpuSequenceNumber = be32(d + pos);
                e.presentationTimeUs = ntpToUs(be64(d + pos + 4));
                out.push_back(e);
            }
        });
    }
    return out;
}

uint32_t videoTimescale(const std::vector<uint8_t>& mptTable, uint16_t videoPacketId)
{
    MptView view;
    if (!parseMpt(mptTable, view))
        return 0;
    uint32_t timescale = 0;
    for (const auto& a : view.assets) {
        if (a.packetId != videoPacketId)
            continue;
        walkDescriptors(mptTable.data() + a.descOffset, a.descLength,
            [&](uint16_t tag, const uint8_t* d, size_t total, size_t hdr) {
            if (tag != kMpuExtendedTimestampTag || timescale != 0)
                return;
            if (hdr < total && (d[hdr] & 1) && hdr + 5 <= total)
                timescale = be32(d + hdr + 1);
        });
    }
    return timescale;
}

std::vector<uint8_t> packPaMptPacket(const std::vector<uint8_t>& mptTable,
                                     uint16_t packetId,
                                     uint16_t contextId,
                                     uint32_t packetSequenceNumber,
                                     uint32_t deliveryTimestamp,
                                     uint8_t paVersion)
{
    // PA message with no table-info entries: the table data follows directly.
    std::vector<uint8_t> pa;
    pa.reserve(8 + mptTable.size());
    putBe16(pa, 0x0000); // messageId = PA
    pa.push_back(paVersion);
    putBe32(pa, static_cast<uint32_t>(1 + mptTable.size())); // after length field
    pa.push_back(0);     // numberOfTables
    pa.insert(pa.end(), mptTable.begin(), mptTable.end());

    const size_t mmtpLen = 12 + 2 + pa.size(); // MMTP hdr + signaling hdr + PA
    const size_t cipLen = 3 + mmtpLen;

    std::vector<uint8_t> pkt;
    pkt.reserve(4 + cipLen);
    pkt.push_back(0x7F);
    pkt.push_back(static_cast<uint8_t>(TlvPacketType::HeaderCompressedIpPacket));
    putBe16(pkt, static_cast<uint16_t>(cipLen));
    putBe16(pkt, static_cast<uint16_t>(contextId << 4));
    pkt.push_back(0x61);
    pkt.push_back(0x00); // MMTP flags
    pkt.push_back(0x02); // payloadType = signaling
    putBe16(pkt, packetId);
    putBe32(pkt, deliveryTimestamp);
    putBe32(pkt, packetSequenceNumber);
    pkt.push_back(0x00); // signaling: not fragmented, no aggregation
    pkt.push_back(0x00); // fragmentCounter
    pkt.insert(pkt.end(), pa.begin(), pa.end());
    return pkt;
}

uint64_t usToNtp(uint64_t us)
{
    const uint64_t total = us + kNtpOffsetUs;
    const uint64_t sec = total / 1000000ULL;
    const uint64_t frac = ((total % 1000000ULL) * 0xFFFFFFFFULL) / 1000000ULL;
    return (sec << 32) | frac;
}

} // namespace Splice

} // namespace MmtTlv
