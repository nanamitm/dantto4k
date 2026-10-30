#include "tsVerifier.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr size_t kPacketSize = 188;
constexpr uint8_t kSyncByte = 0x47;
constexpr uint16_t kNullPid = 0x1FFF;
constexpr uint64_t kTimestampMask = (1ULL << 33) - 1;
// Stream id, PES header up to its 255 byte maximum, and the start of the payload.
constexpr size_t kPesHeaderCapacity = 9 + 255 + 4;

// PSI/SI PIDs of an ARIB TS.
constexpr std::array<uint16_t, 7> kSectionPids = { 0x0000, 0x0010, 0x0011, 0x0012, 0x0014, 0x0024, 0x0029 };

constexpr uint8_t kStreamTypePrivateSection = 0x05;
constexpr uint8_t kStreamTypeAac = 0x0F;
constexpr uint8_t kStreamTypeAacLatm = 0x11;
constexpr uint8_t kStreamTypeHevc = 0x24;

constexpr uint8_t kTablePat = 0x00;
constexpr uint8_t kTablePmt = 0x02;
constexpr uint8_t kTableTot = 0x73;

constexpr std::array<uint32_t, 256> makeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
        }
        table[i] = crc;
    }
    return table;
}

constexpr auto kCrcTable = makeCrcTable();

// CRC-32/MPEG-2 over a section including its CRC field is zero when intact.
bool sectionCrcIsValid(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc = (crc << 8) ^ kCrcTable[((crc >> 24) ^ data[i]) & 0xFF];
    }
    return crc == 0;
}

bool isMediaStreamId(uint8_t streamId) {
    return (streamId >= 0xC0 && streamId <= 0xEF);
}

// Streams whose PES has no optional header, ISO/IEC 13818-1 table 2-21.
bool hasNoPesOptionalHeader(uint8_t streamId) {
    switch (streamId) {
    case 0xBC: case 0xBE: case 0xBF: case 0xF0: case 0xF1: case 0xF2: case 0xF8: case 0xFF:
        return true;
    default:
        return false;
    }
}

uint64_t readTimestamp(const uint8_t* p) {
    return (static_cast<uint64_t>((p[0] >> 1) & 0x07) << 30) |
        (static_cast<uint64_t>(p[1]) << 22) |
        (static_cast<uint64_t>(p[2] >> 1) << 15) |
        (static_cast<uint64_t>(p[3]) << 7) |
        (static_cast<uint64_t>(p[4]) >> 1);
}

// Signed distance on the 33 bit timestamp circle.
int64_t timestampDelta(uint64_t current, uint64_t previous) {
    const uint64_t delta = (current - previous) & kTimestampMask;
    return delta >= (1ULL << 32) ? static_cast<int64_t>(delta) - (1LL << 33) : static_cast<int64_t>(delta);
}

std::string format(const char* pattern, auto... values) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), pattern, values...);
    return buffer;
}

double seconds(uint64_t value90k) {
    return static_cast<double>(value90k) / 90000.0;
}

}

TsVerifier::TsVerifier() {
    for (uint16_t pid : kSectionPids) {
        state(pid).kind = PidKind::Section;
    }
}

TsVerifier::~TsVerifier() = default;

TsVerifier::PidState& TsVerifier::state(uint16_t pid) {
    auto& entry = pids[pid];
    if (!entry) {
        entry = std::make_unique<PidState>();
    }
    return *entry;
}

const TsVerifier::PidStats* TsVerifier::getPid(uint16_t pid) const {
    if (pid >= pids.size() || !pids[pid] || pids[pid]->stats.packets == 0) {
        return nullptr;
    }
    return &pids[pid]->stats;
}

void TsVerifier::feed(const uint8_t* data, size_t size) {
    while (size > 0) {
        if (!carry.empty()) {
            const size_t take = std::min(kPacketSize - carry.size(), size);
            carry.insert(carry.end(), data, data + take);
            data += take;
            size -= take;
            if (carry.size() < kPacketSize) {
                return;
            }
            if (carry[0] == kSyncByte) {
                processPacket(carry.data());
                inSyncLoss = false;
                carry.clear();
                continue;
            }
            // Keep what follows: it may hold the next packet boundary.
            if (!inSyncLoss) {
                ++totals.syncLosses;
                inSyncLoss = true;
            }
            auto next = std::find(carry.begin() + 1, carry.end(), kSyncByte);
            carry.erase(carry.begin(), next);
            continue;
        }

        if (data[0] != kSyncByte) {
            if (!inSyncLoss) {
                ++totals.syncLosses;
                inSyncLoss = true;
            }
            const auto* next = std::find(data + 1, data + size, kSyncByte);
            size -= static_cast<size_t>(next - data);
            data = next;
            continue;
        }

        if (size < kPacketSize) {
            carry.assign(data, data + size);
            return;
        }
        // While resynchronizing, a lone 0x47 inside a packet is not a boundary.
        if (inSyncLoss && size > kPacketSize && data[kPacketSize] != kSyncByte) {
            ++data;
            --size;
            continue;
        }
        processPacket(data);
        inSyncLoss = false;
        data += kPacketSize;
        size -= kPacketSize;
    }
}

void TsVerifier::processPacket(const uint8_t* p) {
    ++totals.packets;
    const uint16_t pid = static_cast<uint16_t>(((p[1] & 0x1F) << 8) | p[2]);
    if (pid == kNullPid) {
        ++totals.nullPackets;
        return;
    }

    PidState& st = state(pid);
    ++st.stats.packets;

    if (p[1] & 0x80) {
        ++totals.transportErrors;
        st.pesDamaged = true;
        st.sectionOpen = false;
        st.section.clear();
        return;
    }

    const uint8_t adaptationControl = (p[3] >> 4) & 0x03;
    if (adaptationControl == 0) {
        ++totals.malformedPackets;
        return;
    }

    const bool hasAdaptation = adaptationControl & 0x02;
    const bool hasPayload = adaptationControl & 0x01;
    size_t offset = 4;
    bool discontinuity = false;
    if (hasAdaptation) {
        const uint8_t length = p[4];
        if ((hasPayload && length > 182) || (!hasPayload && length != 183)) {
            ++totals.malformedPackets;
            return;
        }
        if (length > 0) {
            discontinuity = p[5] & 0x80;
        }
        offset += 1 + length;
        if (discontinuity) {
            st.haveTimestamp = false;
            st.havePcr = false;
        }
        if (length >= 7 && (p[5] & 0x10)) {
            processPcr(st, p + 5);
        }
    }

    if (!hasPayload) {
        return;
    }

    const uint8_t cc = p[3] & 0x0F;
    if (st.haveCc && !discontinuity) {
        if (cc == st.lastCc) {
            // One repeat of a packet is allowed; its payload is not new data.
            if (!st.lastWasDuplicate) {
                st.lastWasDuplicate = true;
                ++st.stats.duplicates;
                return;
            }
            ++st.stats.ccErrors;
            st.pesDamaged = true;
            st.sectionOpen = false;
            st.section.clear();
        }
        else if (cc != ((st.lastCc + 1) & 0x0F)) {
            ++st.stats.ccErrors;
            st.pesDamaged = true;
            st.sectionOpen = false;
            st.section.clear();
        }
    }
    st.lastWasDuplicate = false;
    st.haveCc = true;
    st.lastCc = cc;

    const bool unitStart = p[1] & 0x40;
    const uint8_t* payload = p + offset;
    const size_t payloadSize = kPacketSize - offset;
    if (st.kind == PidKind::Section) {
        processSectionPayload(pid, st, payload, payloadSize, unitStart);
    }
    else {
        processPesPayload(st, payload, payloadSize, unitStart);
    }
}

void TsVerifier::processPcr(PidState& st, const uint8_t* flags) {
    const uint64_t base =
        (static_cast<uint64_t>(flags[1]) << 25) |
        (static_cast<uint64_t>(flags[2]) << 17) |
        (static_cast<uint64_t>(flags[3]) << 9) |
        (static_cast<uint64_t>(flags[4]) << 1) |
        (static_cast<uint64_t>(flags[5]) >> 7);

    ++st.stats.pcrCount;
    if (st.havePcr) {
        const int64_t delta = timestampDelta(base, st.lastPcr);
        if (delta < 0) {
            ++st.stats.pcrBackward;
        }
        else {
            const auto interval = static_cast<uint64_t>(delta);
            st.stats.pcrSpan90k += interval;
            st.stats.largestPcrInterval90k = std::max(st.stats.largestPcrInterval90k, interval);
            if (interval > kPcrProblemInterval90k) {
                ++st.stats.pcrOverLimit;
            }
            else if (interval > kPcrWarningInterval90k) {
                ++st.stats.pcrOverWarning;
            }
        }
    }
    st.havePcr = true;
    st.lastPcr = base;
}

void TsVerifier::processSectionPayload(uint16_t pid, PidState& st, const uint8_t* payload, size_t size, bool unitStart) {
    if (!unitStart) {
        if (st.sectionOpen) {
            appendSectionData(pid, st, payload, size);
        }
        return;
    }

    if (size == 0 || static_cast<size_t>(payload[0]) + 1 > size) {
        ++totals.malformedPackets;
        st.sectionOpen = false;
        st.section.clear();
        return;
    }

    const size_t pointer = payload[0];
    if (st.sectionOpen && pointer > 0) {
        appendSectionData(pid, st, payload + 1, pointer);
    }
    if (st.sectionOpen && !st.section.empty()) {
        ++st.stats.incompleteSections;
    }
    st.section.clear();
    st.sectionOpen = true;
    appendSectionData(pid, st, payload + 1 + pointer, size - 1 - pointer);
}

void TsVerifier::appendSectionData(uint16_t pid, PidState& st, const uint8_t* data, size_t size) {
    st.section.insert(st.section.end(), data, data + size);
    size_t offset = 0;
    while (offset < st.section.size()) {
        // table_id 0xFF is stuffing up to the end of the packet.
        if (st.section[offset] == 0xFF) {
            st.section.clear();
            st.sectionOpen = false;
            return;
        }
        if (st.section.size() - offset < 3) {
            break;
        }
        const size_t length = 3 + (((st.section[offset + 1] & 0x0F) << 8) | st.section[offset + 2]);
        if (st.section.size() - offset < length) {
            break;
        }
        processSection(pid, st, st.section.data() + offset, length);
        offset += length;
    }
    if (offset > 0) {
        st.section.erase(st.section.begin(), st.section.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

void TsVerifier::processSection(uint16_t pid, PidState& st, const uint8_t* section, size_t size) {
    ++st.stats.sections;
    ++totals.sections;
    const uint8_t tableId = section[0];
    ++tableCounts[tableId];

    const bool longForm = section[1] & 0x80;
    if (longForm || tableId == kTableTot) {
        if (size < 7 || !sectionCrcIsValid(section, size)) {
            ++st.stats.crcErrors;
            return;
        }
    }

    if (pid == 0 && tableId == kTablePat) {
        processPat(section, size);
    }
    else if (tableId == kTablePmt) {
        processPmt(pid, section, size);
    }
}

void TsVerifier::processPat(const uint8_t* section, size_t size) {
    if (size < 12) {
        return;
    }
    patSeen = true;
    for (size_t pos = 8; pos + 4 <= size - 4; pos += 4) {
        const uint16_t programNumber = static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);
        const uint16_t pmtPid = static_cast<uint16_t>(((section[pos + 2] & 0x1F) << 8) | section[pos + 3]);
        if (programNumber == 0) {
            continue;
        }
        Program& program = programs[programNumber];
        program.pmtPid = pmtPid;
        state(pmtPid).kind = PidKind::Section;
    }
}

void TsVerifier::processPmt(uint16_t pid, const uint8_t* section, size_t size) {
    if (size < 16) {
        return;
    }
    const uint16_t programNumber = static_cast<uint16_t>((section[3] << 8) | section[4]);
    Program& program = programs[programNumber];
    program.pmtPid = pid;
    program.pmtSeen = true;
    program.pcrPid = static_cast<uint16_t>(((section[8] & 0x1F) << 8) | section[9]);

    const size_t programInfoLength = ((section[10] & 0x0F) << 8) | section[11];
    size_t pos = 12 + programInfoLength;
    const size_t end = size - 4;
    while (pos + 5 <= end) {
        const uint8_t streamType = section[pos];
        const uint16_t esPid = static_cast<uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
        const size_t esInfoLength = ((section[pos + 3] & 0x0F) << 8) | section[pos + 4];
        const size_t descriptorsEnd = std::min(pos + 5 + esInfoLength, end);

        PidState& es = state(esPid);
        es.stats.streamType = streamType;
        es.stats.listedInPmt = true;
        es.kind = streamType == kStreamTypePrivateSection ? PidKind::Section : PidKind::Pes;

        for (size_t d = pos + 5; d + 2 <= descriptorsEnd; d += 2 + static_cast<size_t>(section[d + 1])) {
            if (section[d] == 0x52 && section[d + 1] >= 1 && d + 3 <= descriptorsEnd) {
                es.stats.componentTag = section[d + 2];
            }
        }
        pos += 5 + esInfoLength;
    }
}

void TsVerifier::processPesPayload(PidState& st, const uint8_t* payload, size_t size, bool unitStart) {
    if (unitStart) {
        closePes(st);
        const bool startCode = size >= 3 && payload[0] == 0 && payload[1] == 0 && payload[2] == 1;
        if (!startCode) {
            // A PID no PMT has named yet may simply not carry PES.
            if (st.kind == PidKind::Pes) {
                ++st.stats.pesHeaderErrors;
            }
            return;
        }
        st.pesOpen = true;
        st.pesDamaged = false;
        st.pesParsed = false;
        st.frameChecked = false;
        st.pesHeader.clear();
        st.pesReceived = 0;
        st.pesDeclaredLength = 0;
        ++st.stats.pes;
    }

    if (!st.pesOpen) {
        return;
    }

    st.pesReceived += size;
    st.stats.pesBytes += size;
    if (st.pesParsed && st.frameChecked) {
        return;
    }
    const size_t take = std::min(size, kPesHeaderCapacity - std::min(kPesHeaderCapacity, st.pesHeader.size()));
    st.pesHeader.insert(st.pesHeader.end(), payload, payload + take);
    if (!st.pesParsed) {
        parsePesHeader(st);
    }
    if (st.pesParsed && !st.frameChecked) {
        checkPesFrame(st);
    }
}

void TsVerifier::closePes(PidState& st) {
    if (!st.pesOpen) {
        return;
    }
    st.pesOpen = false;
    if (st.pesDamaged) {
        return;
    }
    if (!st.pesParsed) {
        ++st.stats.pesHeaderErrors;
        return;
    }
    // Length zero means unbounded, which ISO/IEC 13818-1 allows only for video.
    const bool lengthKnown = st.pesDeclaredLength != 0;
    if (lengthKnown && st.pesReceived != static_cast<uint64_t>(st.pesDeclaredLength) + 6) {
        ++st.stats.pesLengthErrors;
    }
}

void TsVerifier::parsePesHeader(PidState& st) {
    const auto& h = st.pesHeader;
    if (h.size() < 6) {
        return;
    }
    const uint8_t streamId = h[3];
    st.pesDeclaredLength = static_cast<uint32_t>((h[4] << 8) | h[5]);

    const bool video = streamId >= 0xE0 && streamId <= 0xEF;
    if (st.pesDeclaredLength == 0 && !video) {
        ++st.stats.pesLengthErrors;
    }

    if (hasNoPesOptionalHeader(streamId)) {
        st.pesParsed = true;
        st.pesPayloadOffset = 6;
        st.frameChecked = true; // Carries no elementary stream to recognize.
        return;
    }

    if (h.size() < 9) {
        return;
    }
    if ((h[6] & 0xC0) != 0x80) {
        ++st.stats.pesHeaderErrors;
        st.pesOpen = false;
        return;
    }
    const size_t headerLength = h[8];
    if (h.size() < 9 + headerLength) {
        return;
    }

    st.pesParsed = true;
    st.pesPayloadOffset = 9 + headerLength;

    const uint8_t timestampFlags = h[7] >> 6;
    const bool hasPts = timestampFlags >= 2;
    const bool hasDts = timestampFlags == 3;
    if (timestampFlags == 1 || (hasPts && headerLength < 5) || (hasDts && headerLength < 10)) {
        ++st.stats.pesHeaderErrors;
        return;
    }

    if (isMediaStreamId(streamId)) {
        if (!hasPts) {
            ++st.stats.missingTimestamps;
        }
        else {
            // Decode order is what must never go backward; PTS reorders with B pictures.
            noteTimestamp(st, readTimestamp(h.data() + (hasDts ? 14 : 9)));
        }
    }
}

void TsVerifier::checkPesFrame(PidState& st) {
    const auto& h = st.pesHeader;
    if (h.size() <= st.pesPayloadOffset) {
        return;
    }
    const size_t available = h.size() - st.pesPayloadOffset;
    const uint8_t* payload = h.data() + st.pesPayloadOffset;

    switch (st.stats.streamType) {
    case kStreamTypeHevc:
        if (available < 4) {
            return;
        }
        if (!(payload[0] == 0 && payload[1] == 0 &&
            (payload[2] == 1 || (payload[2] == 0 && payload[3] == 1)))) {
            ++st.stats.frameErrors;
        }
        break;
    case kStreamTypeAac:
        if (available < 2) {
            return;
        }
        if (payload[0] != 0xFF || (payload[1] & 0xF0) != 0xF0) {
            ++st.stats.frameErrors;
        }
        break;
    case kStreamTypeAacLatm:
        if (available < 2) {
            return;
        }
        if (payload[0] != 0x56 || (payload[1] & 0xE0) != 0xE0) {
            ++st.stats.frameErrors;
        }
        break;
    default:
        break;
    }
    st.frameChecked = true;
}

void TsVerifier::noteTimestamp(PidState& st, uint64_t timestamp) {
    if (st.haveTimestamp) {
        const int64_t delta = timestampDelta(timestamp, st.lastTimestamp);
        if (delta < 0) {
            ++st.stats.timestampsBackward;
        }
        else {
            const auto gap = static_cast<uint64_t>(delta);
            st.stats.timestampSpan90k += gap;
            st.stats.largestTimestampGap90k = std::max(st.stats.largestTimestampGap90k, gap);
            if (gap > kTimestampGap90k) {
                ++st.stats.timestampGaps;
            }
        }
    }
    st.haveTimestamp = true;
    st.lastTimestamp = timestamp;
}

uint64_t TsVerifier::countProblems() const {
    uint64_t problems = totals.syncLosses + totals.transportErrors + totals.malformedPackets;
    if (totals.packets == 0 || !patSeen) {
        ++problems;
    }

    for (const auto& entry : pids) {
        if (!entry) {
            continue;
        }
        const PidStats& s = entry->stats;
        problems += s.ccErrors + s.crcErrors + s.incompleteSections + s.pesHeaderErrors + s.pesLengthErrors +
            s.frameErrors + s.missingTimestamps + s.timestampsBackward + s.pcrBackward + s.pcrOverLimit;
    }

    for (const auto& [number, program] : programs) {
        if (!program.pmtSeen) {
            ++problems;
            continue;
        }
        const PidStats* pcr = getPid(program.pcrPid);
        if (program.pcrPid == kNullPid || !pcr || pcr->pcrCount == 0) {
            ++problems;
        }
    }
    return problems;
}

uint64_t TsVerifier::countWarnings() const {
    uint64_t warnings = 0;
    for (const auto& entry : pids) {
        if (entry) {
            warnings += entry->stats.pcrOverWarning + entry->stats.timestampGaps;
            // Scrambled input, for one, leaves a program with a PMT and no media.
            if (entry->stats.listedInPmt && entry->stats.packets == 0) {
                ++warnings;
            }
        }
    }
    return warnings;
}

void TsVerifier::print(std::ostream& out) const {
    out << format("packets: %llu (%llu bytes), null %llu, sync losses %llu, transport errors %llu, malformed %llu\n",
        static_cast<unsigned long long>(totals.packets),
        static_cast<unsigned long long>(totals.packets * kPacketSize),
        static_cast<unsigned long long>(totals.nullPackets),
        static_cast<unsigned long long>(totals.syncLosses),
        static_cast<unsigned long long>(totals.transportErrors),
        static_cast<unsigned long long>(totals.malformedPackets));

    out << format("sections: %llu", static_cast<unsigned long long>(totals.sections));
    if (!tableCounts.empty()) {
        out << ", table ids:";
        for (const auto& [tableId, count] : tableCounts) {
            out << format(" 0x%02x=%llu", tableId, static_cast<unsigned long long>(count));
        }
    }
    out << '\n';

    if (!patSeen) {
        out << "PAT: none\n";
    }
    for (const auto& [number, program] : programs) {
        if (program.pmtSeen) {
            out << format("program 0x%04x: PMT PID 0x%04x, PCR PID 0x%04x\n", number, program.pmtPid, program.pcrPid);
        }
        else {
            out << format("program 0x%04x: PMT PID 0x%04x, PMT never seen\n", number, program.pmtPid);
        }
    }

    for (uint16_t pid = 0; pid < pids.size(); ++pid) {
        const PidStats* s = getPid(pid);
        if (s && s->pcrCount > 0) {
            out << format("PCR PID 0x%04x: %llu values, span %.3f s, largest interval %.3f s, over 100 ms %llu, over 1 s %llu, backward %llu\n",
                pid, static_cast<unsigned long long>(s->pcrCount), seconds(s->pcrSpan90k),
                seconds(s->largestPcrInterval90k),
                static_cast<unsigned long long>(s->pcrOverWarning + s->pcrOverLimit),
                static_cast<unsigned long long>(s->pcrOverLimit),
                static_cast<unsigned long long>(s->pcrBackward));
        }
    }

    out << "\n   PID type  tag   packets      PES    sections  cc_err crc_err sec_err hdr_err len_err frame_err no_ts ts_back gaps span(s)\n";
    std::set<uint16_t> unlisted;
    for (uint16_t pid = 0; pid < pids.size(); ++pid) {
        const PidStats* s = getPid(pid);
        if (!s) {
            continue;
        }
        const std::string tag = s->componentTag >= 0 ? format("0x%02x", s->componentTag) : std::string("-");
        out << format(" %04x  0x%02x %-5s %8llu %8llu %11llu %7llu %7llu %7llu %7llu %7llu %9llu %5llu %7llu %4llu %7.3f\n",
            pid, s->streamType, tag.c_str(),
            static_cast<unsigned long long>(s->packets),
            static_cast<unsigned long long>(s->pes),
            static_cast<unsigned long long>(s->sections),
            static_cast<unsigned long long>(s->ccErrors),
            static_cast<unsigned long long>(s->crcErrors),
            static_cast<unsigned long long>(s->incompleteSections),
            static_cast<unsigned long long>(s->pesHeaderErrors),
            static_cast<unsigned long long>(s->pesLengthErrors),
            static_cast<unsigned long long>(s->frameErrors),
            static_cast<unsigned long long>(s->missingTimestamps),
            static_cast<unsigned long long>(s->timestampsBackward),
            static_cast<unsigned long long>(s->timestampGaps),
            seconds(s->timestampSpan90k));
        if (pids[pid]->kind == PidKind::Unknown && !s->listedInPmt && s->pcrCount == 0) {
            unlisted.insert(pid);
        }
    }
    for (const auto& [number, program] : programs) {
        unlisted.erase(program.pcrPid);
    }
    if (!unlisted.empty()) {
        out << "\nPIDs in no PMT:";
        for (uint16_t pid : unlisted) {
            out << format(" 0x%04x", pid);
        }
        out << '\n';
    }

    std::string silent;
    for (uint16_t pid = 0; pid < pids.size(); ++pid) {
        if (pids[pid] && pids[pid]->stats.listedInPmt && pids[pid]->stats.packets == 0) {
            silent += format(" 0x%04x", pid);
        }
    }
    if (!silent.empty()) {
        out << "\nPIDs in a PMT with no data:" << silent << '\n';
    }

    out << format("\nproblems: %llu, warnings: %llu\n",
        static_cast<unsigned long long>(countProblems()),
        static_cast<unsigned long long>(countWarnings()));
}
