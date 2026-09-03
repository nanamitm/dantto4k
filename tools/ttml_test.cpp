// ttml_test.cpp - checks for the TTML parser/resolver/converter paths that no
// capture on hand exercises, so a regression in them would otherwise be
// invisible. Every case here failed before the fix it guards.
//
// Build (x64 Native Tools Command Prompt, from this directory):
//
//   cl /nologo /O2 /EHsc /std:c++20 /I..\src ttml_test.cpp ^
//      ..\src\ttml\parser.cpp ..\src\ttml\resolver.cpp ..\src\ttml\style.cpp ^
//      ..\src\ttml\b24_converter.cpp ..\src\ttml\drcs.cpp ^
//      ..\src\aribTextEncoder.cpp ..\src\aribCharsetEncoder.cpp ^
//      ..\src\aribCharsetTables.cpp ..\src\b24Color.cpp ..\src\config.cpp ^
//      ..\src\additionalAribSubtitleInfo.cpp ..\src\ntpTimestamp.cpp ^
//      ..\src\timebase.cpp ..\src\pugixml.cpp /Fe:ttml_test.exe
//
// Exits non-zero when a check fails, so it can be dropped into a build step.
#include "ttml/parser.h"
#include "ttml/resolver.h"
#include "ttml/b24_converter.h"
#include <cstdio>
#include <string>

using namespace arib::ttml;

void subtitleDebugLog(const std::string&) {}

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

static std::optional<resolved::Document> run(const std::string& xml) {
    auto p = parse(xml, SyncMode::Sync);
    if (p.has_error() || !p.document) {
        std::printf("    parse error: %s\n", p.error ? p.error->message.c_str() : "(none)");
        return std::nullopt;
    }
    auto r = resolve(*p.document, SyncMode::Sync);
    if (r.has_error() || !r.document) {
        std::printf("    resolve error: %s\n", r.error ? r.error->message.c_str() : "(none)");
        return std::nullopt;
    }
    return std::move(*r.document);
}

static B24ConvertResult convert(const resolved::Document& doc) {
    auto conv = convert_to_b24(doc, SyncMode::Sync,
                               MmtTlv::SubtitleResolution::Resolution4K, {});
    if (conv.error) std::printf("    b24 error: %s\n", conv.error->message.c_str());
    return conv;
}

static const char* kHead =
    "<tt xmlns='http://www.w3.org/ns/ttml' xmlns:tts='http://www.w3.org/ns/ttml#styling'>"
    "<head><styling>"
    "<style xml:id='base' tts:fontSize='36px 36px'/>"
    "<style xml:id='pos' tts:origin='120px 800px' tts:extent='1680px 200px'/>"
    "</styling><layout>"
    "<region xml:id='r' style='pos'/>"
    "</layout></head>";

int main() {
    std::printf("1. <br/> directly inside <p>\n");
    {
        auto doc = run(std::string(kHead) +
            "<body><div><p region='r'>"
            "<span style='base'>line one</span><br/><span style='base'>line two</span>"
            "</p></div></body></tt>");
        check(doc.has_value(), "document parses");
        if (doc && doc->division) {
            const auto& spans = doc->division->paragraphs.at(0).spans;
            check(spans.size() == 3, "three spans (text, break, text)");
            check(spans.size() == 3 && spans[1].content.size() == 1 &&
                  std::holds_alternative<ast::LineBreak>(spans[1].content[0]),
                  "middle span holds the line break");
            auto conv = convert(*doc);
            check(!conv.has_error(), "converts to B24 without error");
            check(!conv.outputs.empty() && !conv.outputs.front().data.empty(),
                  "paragraph survives conversion (not dropped for a missing font size)");
        }
    }

    std::printf("2. inline tts:* on content elements\n");
    {
        auto doc = run(std::string(kHead) +
            "<body><div><p region='r' tts:color='#00FF00'>"
            "<span style='base' tts:color='#FF0000' tts:fontWeight='bold'>x</span>"
            "<span style='base'>y</span>"
            "</p></div></body></tt>");
        check(doc.has_value(), "document parses");
        if (doc && doc->division) {
            const auto& spans = doc->division->paragraphs.at(0).spans;
            const auto& a = spans.at(0).style;
            const auto& b = spans.at(1).style;
            check(a.color && a.color->r() == 0xFF && a.color->g() == 0x00,
                  "span inline tts:color wins (#FF0000)");
            check(a.font_weight == StyleFontWeightValue::Bold,
                  "span inline tts:fontWeight applied");
            check(b.color && b.color->r() == 0x00 && b.color->g() == 0xFF,
                  "second span inherits the <p> inline colour (#00FF00)");
            check(a.font_size && a.font_size->x == 36.0,
                  "referenced style still supplies fontSize");
        }
    }

    std::printf("3. tts:origin/extent via a referenced <style>\n");
    {
        auto doc = run(std::string(kHead) +
            "<body><div><p region='r'><span style='base'>z</span></p></div></body></tt>");
        check(doc.has_value(), "document parses");
        if (doc && doc->division) {
            const auto& region = doc->division->paragraphs.at(0).region;
            check(region.has_value(), "region resolved");
            check(region && region->style.origin && region->style.origin->x == 120.0,
                  "origin carried over from the referenced style");
            check(region && region->style.extent && region->style.extent->x == 1680.0,
                  "extent carried over from the referenced style");
            check(!convert(*doc).has_error(), "no 'span has no effective region' error");
        }
    }

    std::printf("4. regression: geometry written directly on <region>\n");
    {
        auto doc = run(
            "<tt xmlns='http://www.w3.org/ns/ttml' xmlns:tts='http://www.w3.org/ns/ttml#styling'>"
            "<head><styling><style xml:id='base' tts:fontSize='36px 36px'/></styling>"
            "<layout><region xml:id='r' tts:origin='10px 20px' tts:extent='100px 50px'/></layout></head>"
            "<body><div><p region='r'><span style='base'>q</span></p></div></body></tt>");
        check(doc.has_value(), "document parses");
        if (doc && doc->division) {
            const auto& region = doc->division->paragraphs.at(0).region;
            check(region && region->style.origin && region->style.origin->y == 20.0,
                  "origin still read from the region element");
            check(!convert(*doc).has_error(), "still converts");
        }
    }

    std::printf("5. regression: <br/> inside a <span> still works\n");
    {
        auto doc = run(std::string(kHead) +
            "<body><div><p region='r'><span style='base'>a<br/>b</span></p></div></body></tt>");
        check(doc.has_value(), "document parses");
        if (doc && doc->division) {
            const auto& spans = doc->division->paragraphs.at(0).spans;
            check(spans.size() == 1 && spans[0].content.size() == 3,
                  "one span with text/break/text");
            check(!convert(*doc).has_error(), "still converts");
        }
    }

    std::printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
