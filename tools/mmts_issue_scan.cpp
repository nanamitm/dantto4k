// mmts_issue_scan.cpp - scans an .mmts capture for the constructs behind the
// known dantto4k defects, so a new recording can be triaged before anyone
// spends time trying to reproduce anything with it.
//
// Every check corresponds to a specific finding. OPEN checks fire on defects
// still present in the code. FIXED checks fire on a capture that would have hit
// a defect that has since been repaired, which is what makes such a capture
// worth keeping as a regression sample - none of the captures on hand when the
// fixes were written exercise them. VERIFY checks fire on features that are
// implemented but were never seen on air; their samples carry the broadcast
// time (MH-TOT, JST) so the scene can be found and looked at in a player.
//
// Build (x64 Native Tools Command Prompt, from this directory):
//
//   cl /nologo /O2 /EHsc /std:c++20 /utf-8 /DWIN32 /DWIN32_LEAN_AND_MEAN ^
//      /D_CRT_SECURE_NO_WARNINGS /I..\src ^
//      /I..\thirdparty\asio\asio\include ^
//      mmts_issue_scan.cpp @sources.rsp /Fe:mmts_issue_scan.exe ^
//      /link winscard.lib gdi32.lib user32.lib ^
//      /MANIFEST:EMBED /MANIFESTINPUT:..\msvc\utf8.manifest
//
// The manifest makes UTF-8 the process code page, so a capture whose name has
// a character outside CP932 can be opened.
//
// Exits 0 when nothing fired, 1 when at least one check did, 2 on a usage or
// input error - so it can gate an "is this capture interesting?" step.
#include "mmtTlvDemuxer.h"
#include "demuxerHandler.h"
#include "mmtStream.h"
#include "mhEit.h"
#include "mhTot.h"
#include "mhExtendedEventDescriptor.h"
// casProxyClient.h pulls in asio, which must see winsock2.h before
// smartCard.h brings in winscard.h and with it windows.h.
#include "casProxyClient.h"
#include "acasHandler.h"
#include "smartCard.h"
#include "config.h"
#include "aribCharsetEncoder.h"
#include "ttml/parser.h"
#include "ttml/resolver.h"
#include "ttml/b24_converter.h"
#include "ttml/drcs.h"
#include "pugixml.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

void subtitleDebugLog(const std::string&) {}

namespace { bool g_dumpDrcs = false; }

namespace {

enum class Category { Open, Fixed, Verify };

struct Check {
    const char* id;
    Category category;
    const char* what;
    unsigned long long hits;
    std::vector<std::string> samples;

    void hit(const std::string& sample) {
        ++hits;
        if (sample.empty() || samples.size() >= 5) {
            return;
        }
        for (const auto& s : samples) {
            if (s == sample) {
                return;
            }
        }
        samples.push_back(sample);
    }
};

enum CheckId {
    kAribPuaDrcs, kNoAribCode, kPStyleRef, kNestedSpan, kOversizedLength, kDrcsUnrasterizable,
    kTtmlRejected, kEedMultiItem, kSubtitleTmd, kMultiCueDoc, kClearOnlyDoc,
    kPLevelContent, kInlineTts, kStyleGeometry,
    kVerticalWriting, kVerticalTcy, kVerticalSmallKana, kHalfWidth,
    kNptReference, kNptEvent, kTimedEvent, kCheckCount
};

Check checks[kCheckCount] = {
    {"arib-pua-drcs", Category::Open, "caption text uses a private-use codepoint the ARIB charset tables cover; is_drcs_codepoint() calls it DRCS anyway, so the caption waits for a glyph the broadcast has no reason to send", 0, {}},
    {"no-arib-code", Category::Fixed, "caption text uses a character no ARIB graphic set has a code for; it used to go out as a code meaning something else, and is now drawn from a font and sent as a DRCS pattern instead", 0, {}},
    {"p-style-ref", Category::Fixed, "<p style=\"...\"> - a paragraph style reference, which the parser used to ignore", 0, {}},
    {"nested-span", Category::Fixed, "<span> inside a <span> - used to be a parse error that discarded the whole document", 0, {}},
    {"oversized-length", Category::Fixed, "a style length large enough to overflow the double-to-integer conversions in the B24 encoder; now clamped", 0, {}},
    {"drcs-unrasterizable", Category::Open, "a received DRCS glyph that produces no pattern, while the statement still designates its code", 0, {}},
    {"ttml-rejected", Category::Open, "a TTML document the parser, resolver or converter rejects - silent in release builds", 0, {}},
    {"eed-multi-item", Category::Fixed, "MH-extended_event_descriptor with 2+ items - the stray i++ wrote only every other one", 0, {}},
    {"subtitle-tmd", Category::Fixed, "subtitle descriptor with no usable reference_start_time - every cue used to be dropped", 0, {}},
    {"multi-cue-doc", Category::Fixed, "TTML yielding 2+ timed outputs - these used to collapse onto a single PTS", 0, {}},
    {"clear-only-doc", Category::Fixed, "TTML yielding only a clear, or a first output with no begin - these used to be discarded", 0, {}},
    {"p-level-content", Category::Fixed, "text or <br/> directly inside <p> - used to be dropped, running the lines together", 0, {}},
    {"inline-tts", Category::Fixed, "tts:* written directly on <p> or <span> - used to be ignored entirely", 0, {}},
    {"style-geometry", Category::Fixed, "tts:origin/tts:extent on a <style> in <head> - used to be dropped, leaving the region with no geometry", 0, {}},
    {"vertical-writing", Category::Verify, "tts:writingMode=\"tbrl\" caption - laid out character by character, punctuation in vertical forms", 0, {}},
    {"vertical-tcy", Category::Verify, "two half-width characters on their own in vertical text - shown side by side in one cell (tate-chu-yoko)", 0, {}},
    {"vertical-small-kana", Category::Verify, "small kana in vertical text - moved to the upper right of the cell", 0, {}},
    {"half-width", Category::Verify, "half-width (MSZ) characters in horizontal text - check their width and spacing", 0, {}},
    {"npt-reference", Category::Verify, "NPT reference message (UTC-NPT reference descriptor in an EMT) - drives setAlarmByUTCNPT/getUTCNPT", 0, {}},
    {"npt-event", Category::Verify, "event message timed by NPT (time_mode 2) - fires at the time the NPT reference gives", 0, {}},
    {"timed-event", Category::Verify, "event message timed by UTC or relative time (time_mode 1, 3, 5) - check it fires on time", 0, {}}
};

// The broadcast time of the last MH-TOT, put in front of VERIFY samples.
std::string g_broadcastTime;

std::string formatJst(uint64_t jst) {
    // 16-bit MJD then hhmmss in BCD (ARIB STD-B10)
    const int mjd = static_cast<int>(jst >> 24);
    const int yp = static_cast<int>((mjd - 15078.2) / 365.25);
    const int mp = static_cast<int>((mjd - 14956.1 - static_cast<int>(yp * 365.25)) / 30.6001);
    const int day = mjd - 14956 - static_cast<int>(yp * 365.25) - static_cast<int>(mp * 30.6001);
    const int k = (mp == 14 || mp == 15) ? 1 : 0;
    const int year = 1900 + yp + k;
    const int month = mp - 1 - k * 12;
    const auto bcd = [&](int shift) { const int v = static_cast<int>((jst >> shift) & 0xFF); return (v >> 4) * 10 + (v & 0xF); };
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, bcd(16), bcd(8), bcd(0));
    return buf;
}

std::string timed(const std::string& sample) {
    return (g_broadcastTime.empty() ? std::string("[time unknown] ") : "[" + g_broadcastTime + "] ") + sample;
}

std::string formatCp(uint32_t cp) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "U+%04X", cp);
    return buf;
}

// The check only says a glyph produced no pattern. Whether that is the
// broadcaster shipping something odd or the path parser giving up needs the
// path itself, so --dump-drcs prints it.
void dumpDrcsGlyph(uint32_t cp, const arib::ttml::DrcsGlyph& glyph) {
    std::printf("  DRCS %s units=%d ascent=%d descent=%d d=%s\n",
                formatCp(cp).c_str(), glyph.unitsPerEm, glyph.ascent, glyph.descent,
                glyph.path.c_str());
}

std::string toUtf8(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// The charset tables decide this, not a list copied out of them: a codepoint the
// tables cover produces bytes, one they do not produces nothing.
//
// This asks arib::charset::encode() rather than arib::text::encode(). The latter
// first runs the gaiji substitutions, which map some private-use codepoints onto
// a replacement - U+E11A becomes the geta mark - so it answers "yes" for
// codepoints the tables cannot actually represent. Those are real DRCS: the
// broadcast ships a glyph for them, and rendering that glyph beats rendering a
// placeholder. Only a codepoint the tables cover is a false DRCS positive.
bool encodableAsArib(uint32_t cp) {
    static std::map<uint32_t, bool> cache;
    const auto it = cache.find(cp);
    if (it != cache.end()) {
        return it->second;
    }
    const bool ok = !arib::charset::encode(toUtf8(cp), arib::charset::EncodeMode::Caption).empty();
    cache.emplace(cp, ok);
    return ok;
}

void collectCodepoints(std::string_view text, std::set<uint32_t>& out) {
    for (size_t i = 0; i < text.size();) {
        const auto c0 = static_cast<unsigned char>(text[i]);
        uint32_t cp = c0;
        size_t len = 1;
        const auto cont = [&](size_t k) { return static_cast<unsigned char>(text[i + k]) & 0x3F; };
        if ((c0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
            cp = ((c0 & 0x1Fu) << 6) | cont(1);
            len = 2;
        }
        else if ((c0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
            cp = ((c0 & 0x0Fu) << 12) | (cont(1) << 6) | cont(2);
            len = 3;
        }
        else if ((c0 & 0xF8) == 0xF0 && i + 3 < text.size()) {
            cp = ((c0 & 0x07u) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
            len = 4;
        }
        out.insert(cp);
        i += len;
    }
}

bool isWhitespaceOnly(std::string_view s) {
    return s.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

// The encoder scales by 960/sourceWidth; 2K sources give the largest factor, so
// a value flagged here overflows for every resolution.
bool overflowsU32(double value) {
    const double scaled = value * (960.0 / 1920.0);
    return !(scaled >= 0.0 && scaled <= 4294967295.0);
}

void checkLengthAttribute(std::string_view name, const char* value) {
    if (name != "tts:extent" && name != "tts:lineHeight" && name != "arib-tt:letter-spacing") {
        return;
    }
    std::string v(value);
    for (auto& ch : v) {
        if (ch == 'p' || ch == 'x') {
            ch = ' ';
        }
    }
    const char* p = v.c_str();
    while (*p) {
        char* end = nullptr;
        const double d = std::strtod(p, &end);
        if (end == p) {
            ++p;
            continue;
        }
        if (overflowsU32(d)) {
            checks[kOversizedLength].hit(std::string(name) + "=" + value);
        }
        p = end;
    }
}

void scanRawXml(const std::string& xml) {
    pugi::xml_document doc;
    if (doc.load_buffer(xml.data(), xml.size()).status != pugi::status_ok) {
        return;
    }

    for (const auto& selected : doc.select_nodes("//*")) {
        const pugi::xml_node n = selected.node();
        const std::string_view name = n.name();

        if (name == "p" && n.attribute("style")) {
            checks[kPStyleRef].hit(n.attribute("style").value());
        }

        if (name == "p") {
            for (auto c = n.first_child(); c; c = c.next_sibling()) {
                if (c.type() == pugi::node_element && std::string_view(c.name()) == "br") {
                    checks[kPLevelContent].hit("<br/> under <p>");
                }
                else if ((c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) &&
                         !isWhitespaceOnly(c.value())) {
                    checks[kPLevelContent].hit("text under <p>");
                }
            }
        }

        if (name == "span") {
            for (auto c = n.first_child(); c; c = c.next_sibling()) {
                if (c.type() == pugi::node_element && std::string_view(c.name()) == "span") {
                    checks[kNestedSpan].hit("nested <span>");
                }
            }
        }

        for (auto a = n.first_attribute(); a; a = a.next_attribute()) {
            const std::string_view an = a.name();
            if ((name == "p" || name == "span") && an.starts_with("tts:")) {
                checks[kInlineTts].hit("<" + std::string(name) + " " + a.name() + ">");
            }
            if (name == "style" && (an == "tts:origin" || an == "tts:extent")) {
                checks[kStyleGeometry].hit(std::string(an));
            }
            checkLengthAttribute(an, a.value());
        }
    }
}

void scanTtmlText(const std::string& xml) {
    // Async mode so an unparsable time expression cannot hide the text.
    auto parsed = arib::ttml::parse(xml, arib::ttml::SyncMode::Async);
    if (parsed.has_error() || !parsed.document || !parsed.document->division) {
        return;
    }

    std::set<uint32_t> codepoints;
    for (const auto& paragraph : parsed.document->division->paragraphs) {
        for (const auto& span : paragraph.spans) {
            for (const auto& content : span.content) {
                if (const auto* text = std::get_if<std::string>(&content)) {
                    collectCodepoints(*text, codepoints);
                }
            }
        }
    }

    for (const uint32_t cp : codepoints) {
        if (arib::ttml::is_drcs_codepoint(cp)) {
            if (encodableAsArib(cp)) {
                checks[kAribPuaDrcs].hit(formatCp(cp));
            }
            continue;
        }
        // Worth counting even though it is handled now: it says how much of a
        // capture leans on the font fallback, which is the only part of the
        // caption that is a picture rather than text.
        if (!arib::charset::canEncodeCaption(static_cast<char32_t>(cp))) {
            checks[kNoAribCode].hit(formatCp(cp) + " " + toUtf8(cp));
        }
    }
}

bool isSmallKana(uint32_t cp) {
    switch (cp) {
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:
    case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308E:
    case 0x3095: case 0x3096:
    case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9:
    case 0x30C3: case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE:
    case 0x30F5: case 0x30F6:
        return true;
    default:
        return cp >= 0x31F0 && cp <= 0x31FF;
    }
}

std::string spanText(const arib::ttml::resolved::Span& span) {
    std::string text;
    for (const auto& content : span.content) {
        if (const auto* t = std::get_if<std::string>(&content)) {
            text += *t;
        }
    }
    return text;
}

size_t countCodepoints(std::string_view text) {
    size_t count = 0;
    for (const auto c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

// The first characters of text, cut on a character boundary.
std::string headText(std::string_view text, size_t characters) {
    size_t count = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80 && count++ == characters) {
            return std::string(text.substr(0, i)) + "...";
        }
    }
    return std::string(text);
}

// What a document uses of the features the converter handles but no capture
// on hand has: vertical writing (with tate-chu-yoko and small kana) and
// half-width characters.
void scanTtmlFeatures(const arib::ttml::resolved::Document& document) {
    if (!document.division) {
        return;
    }
    for (const auto& paragraph : document.division->paragraphs) {
        std::string paragraphText;
        for (const auto& span : paragraph.spans) {
            paragraphText += spanText(span);
        }
        const std::string sample = timed(headText(paragraphText, 20));
        for (const auto& span : paragraph.spans) {
            const auto& region = span.region ? span.region : paragraph.region;
            const bool vertical = region && region->style.writing_mode == arib::ttml::StyleWritingModeValue::Tbrl;
            const auto& size = span.style.font_size;
            const bool halfWidth = size && size->x * 2 == size->y;
            const std::string text = spanText(span);
            if (text.empty()) {
                continue;
            }
            if (vertical) {
                checks[kVerticalWriting].hit(sample);
                if (halfWidth && countCodepoints(text) == 2) {
                    checks[kVerticalTcy].hit(timed(text) + " in " + headText(paragraphText, 20));
                }
                std::set<uint32_t> codepoints;
                collectCodepoints(text, codepoints);
                for (const auto cp : codepoints) {
                    if (isSmallKana(cp)) {
                        checks[kVerticalSmallKana].hit(sample);
                        break;
                    }
                }
            }
            else if (halfWidth) {
                checks[kHalfWidth].hit(timed(headText(text, 12)) + " in " + headText(paragraphText, 20));
            }
        }
    }
}

// An EMT section (ARIB STD-B60 11.2.1.1): NPT reference messages and event
// messages timed by NPT, UTC or relative time.
void scanEmt(uint16_t packetId, const uint8_t* data, size_t size) {
    if (size < 12 || data[0] != 0xA6) {
        return;
    }
    const size_t sectionSize = 3 + (((data[1] & 0x0F) << 8) | data[2]);
    if (sectionSize > size || sectionSize < 12) {
        return;
    }
    char where[32];
    std::snprintf(where, sizeof(where), "packet_id 0x%04X", packetId);
    const uint8_t* p = data + 8;
    const uint8_t* end = data + sectionSize - 4;
    while (p + 3 <= end) {
        const uint16_t tag = static_cast<uint16_t>((p[0] << 8) | p[1]);
        size_t length;
        const uint8_t* body;
        if (tag >= 0xF000) {
            if (p + 4 > end) break;
            length = static_cast<size_t>((p[2] << 8) | p[3]);
            body = p + 4;
        }
        else {
            length = p[2];
            body = p + 3;
        }
        if (body + length > end) {
            break;
        }
        if (tag == 0x8021 && length >= 17) {
            uint32_t nptSeconds = (body[8] << 24) | (body[9] << 16) | (body[10] << 8) | body[11];
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s NPT %u s, scale %u, leap %u", where, nptSeconds,
                          (body[16] >> 4) & 3, body[16] >> 6);
            checks[kNptReference].hit(timed(buf));
        }
        else if (tag == 0xF003 && length >= 3) {
            const uint8_t timeMode = body[2];
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s time_mode %u", where, timeMode);
            if (timeMode == 2) {
                checks[kNptEvent].hit(timed(buf));
            }
            else if (timeMode == 1 || timeMode == 3 || timeMode == 5) {
                checks[kTimedEvent].hit(timed(buf));
            }
        }
        p = body + length;
    }
}

// Feeds synthetic documents through the detectors, so the scanner is known to
// fire rather than merely known to stay quiet on captures that contain nothing
// - every capture on hand is quiet, which on its own proves little. The checks
// that need a real stream are listed as uncovered at the end.
int runSelfTest() {
    static const char* kHead =
        "<tt xmlns='http://www.w3.org/ns/ttml' xmlns:tts='http://www.w3.org/ns/ttml#styling'>"
        "<head><styling><style xml:id='s' tts:fontSize='36px 36px'/></styling>"
        "<layout><region xml:id='r' tts:origin='0px 0px' tts:extent='100px 50px'/></layout></head>";

    struct Case {
        CheckId id;
        const char* name;
        const char* body;
    };
    const Case cases[] = {
        {kPStyleRef, "p-style-ref",
         "<body><div><p region='r' style='s'><span style='s'>x</span></p></div></body></tt>"},
        {kNestedSpan, "nested-span",
         "<body><div><p region='r'><span style='s'>a<span style='s'>b</span></span></p></div></body></tt>"},
        {kOversizedLength, "oversized-length",
         "<body><div><p region='r'><span style='s' tts:lineHeight='1e30px'>x</span></p></div></body></tt>"},
        {kInlineTts, "inline-tts",
         "<body><div><p region='r'><span style='s' tts:color='#FF0000'>x</span></p></div></body></tt>"},
        {kPLevelContent, "p-level-content",
         "<body><div><p region='r'><span style='s'>a</span><br/><span style='s'>b</span></p></div></body></tt>"}
    };

    int failed = 0;
    const auto report = [&](bool ok, const char* name) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) {
            ++failed;
        }
    };

    for (const auto& c : cases) {
        const auto before = checks[c.id].hits;
        scanRawXml(std::string(kHead) + c.body);
        report(checks[c.id].hits > before, c.name);
    }

    {
        const auto before = checks[kStyleGeometry].hits;
        scanRawXml("<tt xmlns:tts='http://www.w3.org/ns/ttml#styling'><head><styling>"
                   "<style xml:id='p' tts:origin='1px 2px' tts:extent='3px 4px'/>"
                   "</styling></head><body/></tt>");
        report(checks[kStyleGeometry].hits > before, "style-geometry");
    }

    // U+840A is a JIS X 0213 level 3 Kanji at 91区6点 of plane 1, which is where
    // ARIB puts an additional symbol instead, so no caption code stands for it.
    // U+83B1, the same Kanji simplified, is an ordinary one at 45区73点.
    {
        const auto before = checks[kNoAribCode].hits;
        scanTtmlText(std::string(kHead) + "<body><div><p region='r'><span style='s'>" +
                     toUtf8(0x840A) + "</span></p></div></body></tt>");
        report(checks[kNoAribCode].hits > before, "no-arib-code fires for a JIS X 0213 level 3 Kanji");
    }
    {
        const auto before = checks[kNoAribCode].hits;
        scanTtmlText(std::string(kHead) + "<body><div><p region='r'><span style='s'>" +
                     toUtf8(0x83B1) + "</span></p></div></body></tt>");
        report(checks[kNoAribCode].hits == before, "no-arib-code stays quiet for a Kanji the tables carry");
    }

    // U+E0D8 sits in the ARIB additional-symbol table, so the encoder can emit
    // it and treating it as DRCS is the defect.
    {
        const auto before = checks[kAribPuaDrcs].hits;
        scanTtmlText(std::string(kHead) + "<body><div><p region='r'><span style='s'>" +
                     toUtf8(0xE0D8) + "</span></p></div></body></tt>");
        report(checks[kAribPuaDrcs].hits > before, "arib-pua-drcs fires for an encodable PUA codepoint");
    }
    {
        const auto before = checks[kAribPuaDrcs].hits;
        scanTtmlText(std::string(kHead) + "<body><div><p region='r'><span style='s'>" +
                     toUtf8(0xF8F0) + "</span></p></div></body></tt>");
        report(checks[kAribPuaDrcs].hits == before, "arib-pua-drcs stays quiet for a real DRCS codepoint");
    }
    // U+E11A is not in the tables; it only survives arib::text::encode() because
    // the gaiji substitutions turn it into a geta mark. Broadcasts do ship a DRCS
    // glyph for it, so calling it a false positive would itself be one.
    {
        const auto before = checks[kAribPuaDrcs].hits;
        scanTtmlText(std::string(kHead) + "<body><div><p region='r'><span style='s'>" +
                     toUtf8(0xE11A) + "</span></p></div></body></tt>");
        report(checks[kAribPuaDrcs].hits == before,
               "arib-pua-drcs stays quiet for a codepoint only the gaiji table maps");
    }

    {
        const auto before = checks[kDrcsUnrasterizable].hits;
        auto glyphs = arib::ttml::parse_svg_glyph_resource(
            "<svg><defs><font><font-face units-per-em='1024'/>"
            "<glyph unicode='&#xF8F0;' d='not a path'/></font></defs></svg>");
        for (const auto& entry : glyphs) {
            arib::ttml::DrcsCodeAllocator allocator;
            if (!allocator.allocate(entry.first)) {
                continue;
            }
            if (arib::ttml::build_drcs_data_unit(allocator.codes(), glyphs).empty()) {
                checks[kDrcsUnrasterizable].hit(formatCp(entry.first));
            }
        }
        report(!glyphs.empty() && checks[kDrcsUnrasterizable].hits > before, "drcs-unrasterizable");
    }

    // VERIFY: vertical writing with tate-chu-yoko and small kana, half-width
    // characters, NPT reference and timed event messages.
    {
        const auto features = [](const std::string& xml) {
            auto parsed = arib::ttml::parse(xml, arib::ttml::SyncMode::Async);
            if (parsed.has_error() || !parsed.document) return;
            auto resolved = arib::ttml::resolve(*parsed.document, arib::ttml::SyncMode::Async);
            if (resolved.has_error() || !resolved.document) return;
            scanTtmlFeatures(*resolved.document);
        };
        const std::string head =
            "<tt xmlns='http://www.w3.org/ns/ttml' xmlns:tts='http://www.w3.org/ns/ttml#styling'><head><styling>"
            "<style xml:id='full' tts:fontSize='144px 144px'/><style xml:id='half' tts:fontSize='72px 144px'/>"
            "</styling><layout><region xml:id='v' tts:origin='3000px 200px' tts:extent='600px 1600px' tts:writingMode='tbrl'/>"
            "<region xml:id='h' tts:origin='0px 1800px' tts:extent='3000px 200px'/></layout></head><body><div>";
        const auto vertical = checks[kVerticalWriting].hits;
        const auto tcy = checks[kVerticalTcy].hits;
        const auto kana = checks[kVerticalSmallKana].hits;
        const auto half = checks[kHalfWidth].hits;
        // vertical "12" + U+3061 U+3087 (cho), horizontal half-width "AB"
        features(head + "<p region='v'><span style='half'>12</span><span style='full'>\xE3\x81\xA1\xE3\x82\x87</span></p>"
                        "<p region='h'><span style='half'>AB</span></p></div></body></tt>");
        report(checks[kVerticalWriting].hits > vertical, "vertical-writing");
        report(checks[kVerticalTcy].hits > tcy, "vertical-tcy");
        report(checks[kVerticalSmallKana].hits > kana, "vertical-small-kana");
        report(checks[kHalfWidth].hits == half + 1, "half-width (horizontal only)");
    }
    {
        // EMT: UTC-NPT reference descriptor, an NPT-timed and a UTC-timed event message
        std::vector<uint8_t> s = { 0xA6, 0xB0, 0x00, 0xFF, 0xFF, 0xC1, 0x00, 0x00,
            0x80, 0x21, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x03, 0xE8, 0, 0, 0, 0, 0x30,
            0xF0, 0x03, 0x00, 0x0E, 0x00, 0x1F, 0x02, 0, 0, 0x03, 0xF2, 0, 0, 0, 0, 0x01, 0x01, 0x02,
            0xF0, 0x03, 0x00, 0x0E, 0x00, 0x1F, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x01, 0x03,
            0, 0, 0, 0 };
        s[2] = static_cast<uint8_t>(s.size() - 3);
        const auto npt = checks[kNptReference].hits;
        const auto nptEvent = checks[kNptEvent].hits;
        const auto timedEvent = checks[kTimedEvent].hits;
        scanEmt(0x9174, s.data(), s.size());
        report(checks[kNptReference].hits > npt, "npt-reference");
        report(checks[kNptEvent].hits > nptEvent, "npt-event");
        report(checks[kTimedEvent].hits > timedEvent, "timed-event");
    }

    std::printf("\n%s (%d failure(s))\n", failed ? "SELFTEST FAILED" : "SELFTEST PASS", failed);
    std::printf("uncovered here, these need a real stream: eed-multi-item, subtitle-tmd,"
                " multi-cue-doc, clear-only-doc, ttml-rejected\n");
    return failed ? 1 : 0;
}

class Scanner : public MmtTlv::DemuxerHandler {
public:
    unsigned long long ttmlDocs{0};
    unsigned long long glyphResources{0};
    unsigned long long eitEvents{0};
    unsigned long long emtSections{0};

    void onSubtitleData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu) override {
        const std::string payload(mfu.data.begin(), mfu.data.end());
        const uint32_t index = stream.getStreamIndex();

        if (mfu.subtitleDataType != 0) {
            ++glyphResources;
            auto glyphs = arib::ttml::parse_svg_glyph_resource(payload);
            for (const auto& entry : glyphs) {
                arib::ttml::DrcsCodeAllocator allocator;
                if (!allocator.allocate(entry.first)) {
                    continue;
                }
                if (arib::ttml::build_drcs_data_unit(allocator.codes(), glyphs).empty()) {
                    checks[kDrcsUnrasterizable].hit(formatCp(entry.first));
                    if (g_dumpDrcs && dumped_.insert(entry.first).second) {
                        dumpDrcsGlyph(entry.first, entry.second);
                    }
                }
            }
            glyphsByStream_[index].insert(glyphs.begin(), glyphs.end());
            return;
        }

        ++ttmlDocs;
        scanRawXml(payload);
        scanTtmlText(payload);

        const auto& info = stream.additionalAribSubtitleInfo();
        if (info) {
            const bool usable = info->tmd == 0b0010 && info->referenceStartTime.seconds != 0;
            if (info->tmd != 0b1111 && !usable) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "tmd=0x%X refSeconds=%u",
                              info->tmd, info->referenceStartTime.seconds);
                checks[kSubtitleTmd].hit(buf);
            }
        }

        const auto sync = stream.isClosedCaption() ? arib::ttml::SyncMode::Sync
                                                   : arib::ttml::SyncMode::Async;
        auto parsed = arib::ttml::parse(payload, sync);
        if (parsed.has_error() || !parsed.document) {
            checks[kTtmlRejected].hit(parsed.error ? parsed.error->message : "parse failed");
            return;
        }
        auto resolved = arib::ttml::resolve(*parsed.document, sync);
        if (resolved.has_error() || !resolved.document) {
            checks[kTtmlRejected].hit(resolved.error ? resolved.error->message : "resolve failed");
            return;
        }
        scanTtmlFeatures(*resolved.document);
        const auto resolution = info ? info->resolution : MmtTlv::SubtitleResolution::Resolution4K;
        auto converted = arib::ttml::convert_to_b24(*resolved.document, sync, resolution,
                                                    glyphsByStream_[index]);
        if (converted.has_error()) {
            checks[kTtmlRejected].hit(converted.error->message);
            return;
        }

        size_t timed = 0;
        for (const auto& out : converted.outputs) {
            if (out.begin) {
                ++timed;
            }
        }
        if (timed >= 2) {
            checks[kMultiCueDoc].hit(std::to_string(converted.outputs.size()) + " outputs");
        }
        // Only the reference-time path discarded an output with no begin; a
        // tmd of 0b1111 gave every output the media clock and kept them all.
        const bool referenceTimed = info && info->tmd != 0b1111;
        if (referenceTimed && !converted.outputs.empty() && !converted.outputs.front().begin) {
            checks[kClearOnlyDoc].hit(converted.outputs.size() == 1 ? "clear-only document"
                                                                   : "first output has no begin");
        }
    }

    void onMhTot(const MmtTlv::MhTot& tot) override {
        g_broadcastTime = formatJst(tot.jstTime);
    }

    void onApplicationTableSection(uint16_t packetId, const uint8_t* data, size_t size) override {
        if (size > 0 && data[0] == 0xA6) {
            ++emtSections;
            scanEmt(packetId, data, size);
        }
    }

    void onMhEit(const MmtTlv::MhEit& eit) override {
        for (const auto& event : eit.events) {
            if (!event) {
                continue;
            }
            ++eitEvents;
            for (const auto& descriptor : event->descriptors.list) {
                if (descriptor->getDescriptorTag() != MmtTlv::MhExtendedEventDescriptor::kDescriptorTag) {
                    continue;
                }
                const auto* eed = static_cast<const MmtTlv::MhExtendedEventDescriptor*>(descriptor.get());
                if (eed->entries.size() >= 2) {
                    checks[kEedMultiItem].hit(std::to_string(eed->entries.size()) + " items");
                }
            }
        }
    }

private:
    std::map<uint32_t, arib::ttml::DrcsGlyphMap> glyphsByStream_;
    std::set<uint32_t> dumped_;
};

} // namespace

// Samples and paths are UTF-8; the console shows them as such while this runs
// and gets its own code page back afterwards.
class ConsoleUtf8 {
public:
    ConsoleUtf8() : previous_(GetConsoleOutputCP()) {
        if (previous_ != 0) {
            SetConsoleOutputCP(CP_UTF8);
        }
    }
    ~ConsoleUtf8() {
        if (previous_ != 0) {
            SetConsoleOutputCP(previous_);
        }
    }
    ConsoleUtf8(const ConsoleUtf8&) = delete;
    ConsoleUtf8& operator=(const ConsoleUtf8&) = delete;
private:
    UINT previous_;
};

int main(int argc, char* argv[]) {
    ConsoleUtf8 consoleUtf8;
    const char* path = nullptr;
    bool descrambled = false;
    std::string reader;
    unsigned long long limitMb = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--selftest") {
            return runSelfTest();
        }
        if (arg == "--dump-drcs") {
            g_dumpDrcs = true;
        }
        else if (arg == "--frontend-descrambled") {
            descrambled = true;
        }
        else if (arg == "--reader" && i + 1 < argc) {
            reader = argv[++i];
        }
        else if (arg == "--limit-mb" && i + 1 < argc) {
            limitMb = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (!path) {
            path = argv[i];
        }
        else {
            std::fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!path) {
        std::fprintf(stderr,
                     "Usage: mmts_issue_scan <input.mmts> [--frontend-descrambled] [--reader NAME] [--limit-mb N]\n"
                     "                        [--dump-drcs]\n"
                     "       mmts_issue_scan --selftest\n");
        return 2;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return 2;
    }

    MmtTlv::MmtTlvDemuxer demuxer;
    Scanner scanner;
    demuxer.setDemuxerHandler(scanner);
    demuxer.setAssumeDescrambled(descrambled);
    if (!descrambled) {
        try {
            auto acas = std::make_unique<AcasHandler>();
            auto card = std::make_unique<LocalSmartCard>();
            card->setSmartCardReaderName(reader.empty() ? config.smartCardReaderName : reader);
            acas->setSmartCard(std::move(card));
            demuxer.setCasHandler(std::move(acas));
        }
        catch (const std::runtime_error& e) {
            std::fprintf(stderr, "smart card: %s\n"
                                 "  (an already decoded capture can be scanned with --frontend-descrambled)\n",
                         e.what());
            return 2;
        }
    }

    constexpr size_t chunk = 5u << 20;
    std::vector<uint8_t> buffer;
    std::vector<uint8_t> readBuffer(chunk);
    unsigned long long consumedTotal = 0;
    bool eof = false;
    while (true) {
        if (!eof && buffer.size() < chunk) {
            input.read(reinterpret_cast<char*>(readBuffer.data()),
                       static_cast<std::streamsize>(readBuffer.size()));
            const auto got = input.gcount();
            if (got > 0) {
                buffer.insert(buffer.end(), readBuffer.data(), readBuffer.data() + got);
            }
            if (got == 0 && input.eof()) {
                eof = true;
            }
        }
        if (buffer.empty() && eof) {
            break;
        }

        MmtTlv::Common::ReadStream stream(buffer);
        while (!stream.isEof()) {
            if (demuxer.demux(stream) == MmtTlv::DemuxStatus::NotEnoughBuffer) {
                break;
            }
        }
        const auto consumed = buffer.size() - stream.leftBytes();
        buffer.erase(buffer.begin(), buffer.begin() + consumed);
        consumedTotal += consumed;
        if (limitMb && consumedTotal >= limitMb * 1024ull * 1024ull) {
            break;
        }
        if (eof && consumed == 0) {
            break;
        }
    }

    std::printf("scanned %.1f MB - %llu TTML document(s), %llu glyph resource(s), %llu EIT event(s), %llu EMT section(s)\n\n",
                consumedTotal / 1048576.0, scanner.ttmlDocs, scanner.glyphResources, scanner.eitEvents,
                scanner.emtSections);

    if (scanner.ttmlDocs == 0 && scanner.eitEvents == 0) {
        std::printf("nothing to inspect - no subtitles or EIT were decoded.\n"
                    "A scrambled capture needs a card; an already decoded one needs --frontend-descrambled.\n");
        return 2;
    }

    int fired = 0;
    const struct {
        Category category;
        const char* title;
    } sections[] = {
        { Category::Open, "OPEN - defects still present in the code:" },
        { Category::Fixed, "FIXED - this capture would have hit these before the fix:" },
        { Category::Verify, "VERIFY - implemented but never seen on air; look at these scenes in a player:" },
    };
    for (const auto& section : sections) {
        std::printf("%s\n", section.title);
        bool any = false;
        for (const auto& check : checks) {
            if (check.category != section.category || check.hits == 0) {
                continue;
            }
            any = true;
            ++fired;
            std::printf("  %-20s %6llu  %s\n", check.id, check.hits, check.what);
            for (const auto& sample : check.samples) {
                std::printf("  %-20s         e.g. %s\n", "", sample.c_str());
            }
        }
        if (!any) {
            std::printf("  (none)\n");
        }
        std::printf("\n");
    }

    std::printf("%d check(s) fired\n", fired);
    return fired ? 1 : 0;
}
