#include "tsVerifier.h"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }

using Bytes = std::vector<uint8_t>;

constexpr uint16_t kPmtPid = 0x1000;
constexpr uint16_t kPcrPid = 0x01FF;
constexpr uint16_t kAudioPid = 0x0110;
constexpr uint16_t kVideoPid = 0x0100;
constexpr uint16_t kSectionPid = 0x0F00;
constexpr uint16_t kCarouselPid = 0x0140;

static uint32_t crc32Mpeg(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
        }
    }
    return crc;
}

static void appendCrc(Bytes& section) {
    const uint32_t crc = crc32Mpeg(section.data(), section.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
        section.push_back(static_cast<uint8_t>(crc >> shift));
    }
}

class Stream {
public:
    Bytes bytes;
    std::array<uint8_t, 8192> cc{};

    void packet(uint16_t pid, bool unitStart, const Bytes& payload, bool discontinuity = false) {
        require(payload.size() <= 182, "test payload too long");
        Bytes p(188, 0xFF);
        p[0] = 0x47;
        p[1] = static_cast<uint8_t>((unitStart ? 0x40 : 0) | (pid >> 8));
        p[2] = static_cast<uint8_t>(pid);
        p[3] = static_cast<uint8_t>(0x30 | (cc[pid] & 0x0F));
        const size_t stuffing = 184 - payload.size();
        p[4] = static_cast<uint8_t>(stuffing - 1);
        p[5] = discontinuity ? 0x80 : 0;
        std::copy(payload.begin(), payload.end(), p.begin() + static_cast<std::ptrdiff_t>(4 + stuffing));
        ++cc[pid];
        bytes.insert(bytes.end(), p.begin(), p.end());
    }

    void section(uint16_t pid, Bytes data) {
        data.insert(data.begin(), 0); // pointer_field
        require(data.size() <= 184, "test section too long");
        Bytes p(188, 0xFF);
        p[0] = 0x47;
        p[1] = static_cast<uint8_t>(0x40 | (pid >> 8));
        p[2] = static_cast<uint8_t>(pid);
        p[3] = static_cast<uint8_t>(0x10 | (cc[pid] & 0x0F));
        std::copy(data.begin(), data.end(), p.begin() + 4);
        ++cc[pid];
        bytes.insert(bytes.end(), p.begin(), p.end());
    }

    void pcr(uint64_t base90k) {
        Bytes p(188, 0xFF);
        p[0] = 0x47;
        p[1] = static_cast<uint8_t>(kPcrPid >> 8);
        p[2] = static_cast<uint8_t>(kPcrPid);
        p[3] = 0x20;
        p[4] = 183;
        p[5] = 0x10;
        p[6] = static_cast<uint8_t>(base90k >> 25);
        p[7] = static_cast<uint8_t>(base90k >> 17);
        p[8] = static_cast<uint8_t>(base90k >> 9);
        p[9] = static_cast<uint8_t>(base90k >> 1);
        p[10] = static_cast<uint8_t>((base90k << 7) | 0x7E);
        p[11] = 0;
        bytes.insert(bytes.end(), p.begin(), p.end());
    }
};

static Bytes patSection(bool corrupt = false) {
    Bytes s = { 0x00, 0xB0, 13, 0x00, 0x01, 0xC1, 0, 0, 0x00, 0x01,
        static_cast<uint8_t>(0xE0 | (kPmtPid >> 8)), static_cast<uint8_t>(kPmtPid) };
    appendCrc(s);
    if (corrupt) {
        s[9] ^= 0x01;
    }
    return s;
}

static Bytes pmtSection() {
    Bytes body = { static_cast<uint8_t>(0xE0 | (kPcrPid >> 8)), static_cast<uint8_t>(kPcrPid), 0xF0, 0,
        0x0F, static_cast<uint8_t>(0xE0 | (kAudioPid >> 8)), static_cast<uint8_t>(kAudioPid), 0xF0, 3, 0x52, 1, 0x10,
        0x24, static_cast<uint8_t>(0xE0 | (kVideoPid >> 8)), static_cast<uint8_t>(kVideoPid), 0xF0, 0,
        0x05, static_cast<uint8_t>(0xE0 | (kSectionPid >> 8)), static_cast<uint8_t>(kSectionPid), 0xF0, 0,
        0x0D, static_cast<uint8_t>(0xE0 | (kCarouselPid >> 8)), static_cast<uint8_t>(kCarouselPid), 0xF0, 0 };
    Bytes s = { 0x02, 0xB0, static_cast<uint8_t>(5 + body.size() + 4), 0x00, 0x01, 0xC1, 0, 0 };
    s.insert(s.end(), body.begin(), body.end());
    appendCrc(s);
    return s;
}

static void putTimestamp(Bytes& out, uint8_t prefix, uint64_t value) {
    out.push_back(static_cast<uint8_t>((prefix << 4) | (((value >> 30) & 7) << 1) | 1));
    out.push_back(static_cast<uint8_t>(value >> 22));
    out.push_back(static_cast<uint8_t>(((value >> 14) & 0xFE) | 1));
    out.push_back(static_cast<uint8_t>(value >> 7));
    out.push_back(static_cast<uint8_t>((value << 1) | 1));
}

static Bytes audioPes(uint64_t pts, int declaredLengthDelta = 0, uint8_t firstByte = 0xFF, uint8_t flags = 0x84) {
    Bytes frame = { firstByte, 0xF1, 0x50, 0x80, 0x01, 0x00, 0xFC, 0xAA, 0xBB };
    Bytes pes = { 0, 0, 1, 0xC0, 0, 0, flags, 0x80, 5 };
    putTimestamp(pes, 2, pts);
    pes.insert(pes.end(), frame.begin(), frame.end());
    const size_t length = pes.size() - 6 + static_cast<size_t>(declaredLengthDelta);
    pes[4] = static_cast<uint8_t>(length >> 8);
    pes[5] = static_cast<uint8_t>(length);
    return pes;
}

static Bytes videoPes(uint64_t pts, uint64_t dts) {
    Bytes pes = { 0, 0, 1, 0xE0, 0, 0, 0x80, 0xC0, 10 };
    putTimestamp(pes, 3, pts);
    putTimestamp(pes, 1, dts);
    pes.insert(pes.end(), { 0, 0, 0, 1, 0x40, 0x01 });
    return pes;
}

static Bytes carouselSection(bool corrupt = false) {
    Bytes s = { 0x3C, 0xB0, 9, 0x00, 0x01, 0xC1, 0, 0 };
    appendCrc(s);
    if (corrupt) {
        s[3] ^= 0x01;
    }
    return s;
}

static Stream cleanStream() {
    Stream s;
    s.section(0, patSection());
    s.section(kPmtPid, pmtSection());
    Bytes app = { 0xC0, 0xB0, 9, 0x00, 0x01, 0xC1, 0, 0 };
    appendCrc(app);
    s.section(kSectionPid, app);
    s.section(kCarouselPid, carouselSection());
    for (uint64_t i = 0; i < 20; ++i) {
        s.pcr(90000 + i * 4500);
        s.packet(kVideoPid, true, videoPes(100000 + i * 3003 + 6006, 100000 + i * 3003));
        s.packet(kAudioPid, true, audioPes(100000 + i * 1920));
    }
    return s;
}

static std::unique_ptr<TsVerifier> verify(const Bytes& bytes) {
    auto verifier = std::make_unique<TsVerifier>();
    verifier->feed(bytes.data(), bytes.size());
    return verifier;
}

static void testClean() {
    const auto stream = cleanStream();
    const auto v = verify(stream.bytes);
    std::ostringstream out;
    v->print(out);
    require(v->countProblems() == 0, out.str().c_str());
    require(v->countWarnings() == 0, "a regular stream must not warn");
    require(v->getTotals().packets == stream.bytes.size() / 188, "every packet must be counted");
    const auto* audio = v->getPid(kAudioPid);
    require(audio && audio->pes == 20 && audio->streamType == 0x0F && audio->componentTag == 0x10 && audio->listedInPmt,
        "PMT must describe the audio PID");
    const auto* video = v->getPid(kVideoPid);
    require(video && video->pes == 20 && video->timestampSpan90k == 19 * 3003, "video must be timed by DTS");
    const auto* pcr = v->getPid(kPcrPid);
    require(pcr && pcr->pcrCount == 20 && pcr->largestPcrInterval90k == 4500, "PCR must be read from the adaptation field");
    const auto* app = v->getPid(kSectionPid);
    require(app && app->sections == 1 && app->crcErrors == 0, "a stream_type 0x05 PID carries sections");
    const auto* carousel = v->getPid(kCarouselPid);
    require(carousel && carousel->sections == 1 && carousel->pesHeaderErrors == 0, "a stream_type 0x0D PID carries sections");
    require(out.str().find("problems: 0") != std::string::npos, "the summary must state the problem count");
}

static void testChunkingAndResync() {
    const auto stream = cleanStream();
    const auto whole = verify(stream.bytes);

    for (size_t chunk : {size_t{1}, size_t{7}, size_t{187}, size_t{188}, size_t{189}, size_t{4096}}) {
        TsVerifier v;
        for (size_t off = 0; off < stream.bytes.size(); off += chunk) {
            v.feed(stream.bytes.data() + off, std::min(chunk, stream.bytes.size() - off));
        }
        require(v.countProblems() == 0 && v.getTotals().packets == whole->getTotals().packets,
            "packet boundaries must not depend on read boundaries");
    }

    auto shifted = stream.bytes;
    const Bytes garbage = { 0x00, 0x47, 0x00 };
    shifted.insert(shifted.begin() + 188 * 5, garbage.begin(), garbage.end());
    const auto v = verify(shifted);
    require(v->getTotals().syncLosses == 1, "a run of garbage is one sync loss");
    require(v->getTotals().packets == whole->getTotals().packets, "packets after the garbage must be recovered");
}

static void expectProblem(const char* what, const std::function<void(Stream&)>& damage,
    const std::function<bool(const TsVerifier&)>& detected) {
    auto stream = cleanStream();
    damage(stream);
    const auto v = verify(stream.bytes);
    if (v->countProblems() == 0 || !detected(*v)) {
        throw std::runtime_error(what);
    }
}

static void testDefects() {
    expectProblem("a missing PAT", [](Stream& s) { s.bytes.erase(s.bytes.begin(), s.bytes.begin() + 188); },
        [](const TsVerifier& v) { return v.getPid(0) == nullptr; });
    expectProblem("a skipped continuity counter", [](Stream& s) { s.cc[kAudioPid] += 3; s.packet(kAudioPid, true, audioPes(200000)); },
        [](const TsVerifier& v) { return v.getPid(kAudioPid)->ccErrors == 1; });
    expectProblem("a corrupt section", [](Stream& s) { s.section(0, patSection(true)); },
        [](const TsVerifier& v) { return v.getPid(0)->crcErrors == 1; });
    expectProblem("a wrong PES length", [](Stream& s) {
        s.packet(kAudioPid, true, audioPes(200000, 40));
        s.packet(kAudioPid, true, audioPes(201920)); },
        [](const TsVerifier& v) { return v.getPid(kAudioPid)->pesLengthErrors == 1; });
    expectProblem("a zero audio PES length", [](Stream& s) { s.packet(kAudioPid, true, Bytes{ 0, 0, 1, 0xC0, 0, 0, 0x80, 0, 0 }); },
        [](const TsVerifier& v) { return v.getPid(kAudioPid)->pesLengthErrors == 1; });
    expectProblem("audio time going backward", [](Stream& s) { s.packet(kAudioPid, true, audioPes(100)); },
        [](const TsVerifier& v) { return v.getPid(kAudioPid)->timestampsBackward == 1; });
    expectProblem("a bad ADTS sync", [](Stream& s) { s.packet(kAudioPid, true, audioPes(200000, 0, 0x12)); },
        [](const TsVerifier& v) { return v.getPid(kAudioPid)->frameErrors == 1; });
    expectProblem("a corrupt carousel section", [](Stream& s) { s.section(kCarouselPid, carouselSection(true)); },
        [](const TsVerifier& v) { return v.getPid(kCarouselPid)->crcErrors == 1; });
    expectProblem("an aligned audio PES without a PTS", [](Stream& s) { s.packet(kAudioPid, true, Bytes{ 0, 0, 1, 0xC0, 0, 6, 0x84, 0, 0, 0xFF, 0xF1, 0x50 }); },
        [](const TsVerifier& v) { return v.getPid(kAudioPid)->missingTimestamps == 1; });
    expectProblem("a PES without a start code", [](Stream& s) { s.packet(kAudioPid, true, Bytes{ 1, 2, 3, 4 }); },
        [](const TsVerifier& v) { return v.getPid(kAudioPid)->pesHeaderErrors == 1; });
    expectProblem("a transport error", [](Stream& s) { s.bytes[188 * 3 + 1] |= 0x80; },
        [](const TsVerifier& v) { return v.getTotals().transportErrors == 1; });
    expectProblem("PCR going backward", [](Stream& s) { s.pcr(1000); },
        [](const TsVerifier& v) { return v.getPid(kPcrPid)->pcrBackward == 1; });
    expectProblem("a very long PCR interval", [](Stream& s) { s.pcr(90000 + 19 * 4500 + 200000); },
        [](const TsVerifier& v) { return v.getPid(kPcrPid)->pcrOverLimit == 1; });

    require(verify({})->countProblems() > 0, "an empty input is not a valid TS");

    Stream noPmt;
    noPmt.section(0, patSection());
    require(verify(noPmt.bytes)->countProblems() == 1, "a program whose PMT never came is a problem");

    Stream noPcr;
    noPcr.section(0, patSection());
    noPcr.section(kPmtPid, pmtSection());
    require(verify(noPcr.bytes)->countProblems() == 1, "a program whose PCR PID has no PCR is a problem");
    const auto silent = verify(noPcr.bytes);
    std::ostringstream silentOut;
    silent->print(silentOut);
    require(silent->countWarnings() == 4, "each PID a PMT lists without data is a warning");
    require(silentOut.str().find("no data: 0x0100 0x0110 0x0140 0x0f00") != std::string::npos, "PIDs without data must be named");
}

static void testTolerated() {
    auto duplicated = cleanStream();
    const Bytes last(duplicated.bytes.end() - 188, duplicated.bytes.end());
    duplicated.bytes.insert(duplicated.bytes.end(), last.begin(), last.end());
    const auto dup = verify(duplicated.bytes);
    require(dup->countProblems() == 0 && dup->getPid(kAudioPid)->duplicates == 1, "one repeated packet is allowed");
    require(dup->getPid(kAudioPid)->pes == 20, "a repeated packet must not count as new data");

    auto marked = cleanStream();
    marked.cc[kAudioPid] += 5;
    marked.packet(kAudioPid, true, audioPes(100, 0), true);
    require(verify(marked.bytes)->countProblems() == 0, "the discontinuity indicator excuses CC and time jumps");

    auto late = cleanStream();
    late.pcr(90000 + 19 * 4500 + 18000);
    const auto warned = verify(late.bytes);
    require(warned->countProblems() == 0 && warned->countWarnings() == 1, "a PCR gap over 100 ms is only a warning");

    auto gap = cleanStream();
    gap.packet(kAudioPid, true, audioPes(100000 + 19 * 1920 + 200000));
    const auto gapped = verify(gap.bytes);
    require(gapped->countProblems() == 0 && gapped->getPid(kAudioPid)->timestampGaps == 1, "a media gap is only a warning");

    // Broadcast audio: frames span PES packets, and only some PES packets carry a PTS.
    auto spanning = cleanStream();
    spanning.packet(kAudioPid, true, audioPes(100000 + 20 * 1920, 0, 0x46, 0x80));
    spanning.packet(kAudioPid, true, Bytes{ 0, 0, 1, 0xC0, 0, 6, 0x80, 0, 0, 0x46, 0x64, 0xB0 });
    const auto spanned = verify(spanning.bytes);
    require(spanned->countProblems() == 0 && spanned->getPid(kAudioPid)->frameErrors == 0 && spanned->getPid(kAudioPid)->missingTimestamps == 0,
        "an unaligned PES may begin mid frame and need not carry a PTS");

    auto cut = cleanStream();
    cut.packet(kCarouselPid, true, Bytes{ 0, 0x3C, 0xB0, 0x40, 1, 2, 3 });
    cut.section(kCarouselPid, carouselSection());
    const auto cutShort = verify(cut.bytes);
    require(cutShort->countProblems() == 0 && cutShort->countWarnings() == 1 && cutShort->getPid(kCarouselPid)->cutCarouselSections == 1,
        "a carousel section cut short without a continuity break is only a warning");

    auto wrap = cleanStream();
    wrap.packet(kAudioPid, true, audioPes(1000));
    wrap.packet(kAudioPid, true, audioPes((1ULL << 33) - 500), true);
    wrap.packet(kAudioPid, true, audioPes(500));
    require(verify(wrap.bytes)->getPid(kAudioPid)->timestampsBackward == 1,
        "time passing the 33 bit wrap point must not count as going backward");

    require(verify(cleanStream().bytes)->getPid(0x1234) == nullptr, "a PID that never appeared has no stats");
}

static void testUnknownPidsAreReported() {
    auto stream = cleanStream();
    stream.packet(0x1234, false, Bytes{ 1, 2, 3 });
    const auto v = verify(stream.bytes);
    std::ostringstream out;
    v->print(out);
    require(v->countProblems() == 0, "a PID in no PMT is reported, not a defect");
    require(out.str().find("PIDs in no PMT: 0x1234") != std::string::npos, "the unlisted PID must be named");
}

int main() {
    try {
        testClean();
        testChunkingAndResync();
        testDefects();
        testTolerated();
        testUnknownPidsAreReported();
        std::puts("PASS: TS verifier detects stream defects and tolerates what the standard allows");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
