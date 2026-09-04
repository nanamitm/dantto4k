#include "ttml/drcs.h"

#include "ttml/parser.h"
#include "aribTextEncoder.h"
#include "b24ControlSet.h"
#include "pugixml.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <variant>

void subtitleDebugLog(const std::string& line);

namespace arib {

namespace ttml {

namespace {

std::string formatCodepoint(uint32_t cp) {
    std::ostringstream ss;
    ss << "U+" << std::uppercase << std::hex << std::setw(cp <= 0xFFFF ? 4 : 6) << std::setfill('0') << cp;
    return ss.str();
}

std::optional<uint32_t> readUtf8Codepoint(std::string_view text, size_t& pos) {
    if (pos >= text.size()) {
        return std::nullopt;
    }

    const auto c0 = static_cast<unsigned char>(text[pos]);
    if (c0 < 0x80) {
        ++pos;
        return c0;
    }
    if ((c0 & 0xE0) == 0xC0 && pos + 1 < text.size()) {
        uint32_t cp = ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(text[pos + 1]) & 0x3F);
        pos += 2;
        return cp;
    }
    if ((c0 & 0xF0) == 0xE0 && pos + 2 < text.size()) {
        uint32_t cp = ((c0 & 0x0F) << 12) |
            ((static_cast<unsigned char>(text[pos + 1]) & 0x3F) << 6) |
            (static_cast<unsigned char>(text[pos + 2]) & 0x3F);
        pos += 3;
        return cp;
    }
    if ((c0 & 0xF8) == 0xF0 && pos + 3 < text.size()) {
        uint32_t cp = ((c0 & 0x07) << 18) |
            ((static_cast<unsigned char>(text[pos + 1]) & 0x3F) << 12) |
            ((static_cast<unsigned char>(text[pos + 2]) & 0x3F) << 6) |
            (static_cast<unsigned char>(text[pos + 3]) & 0x3F);
        pos += 4;
        return cp;
    }

    ++pos;
    return std::nullopt;
}

template <typename Callback>
void forEachUtf8Codepoint(std::string_view text, Callback callback) {
    size_t pos = 0;
    while (pos < text.size()) {
        auto cp = readUtf8Codepoint(text, pos);
        if (cp) {
            callback(*cp);
        }
    }
}

// Walks the rendered text of a TTML document. Parsing is done in Async mode so
// that a malformed time expression cannot hide the text from us - only timing
// parsing differs between the two modes. Returns false when the document could
// not be parsed at all, in which case the caller falls back to a raw scan.
template <typename Callback>
bool forEachTtmlTextCodepoint(const std::string& input, Callback callback) {
    auto parsed = parse(input, SyncMode::Async);
    if (parsed.has_error() || !parsed.document) {
        return false;
    }

    const auto& division = parsed.document->division;
    if (!division) {
        return true;
    }

    for (const auto& paragraph : division->paragraphs) {
        for (const auto& span : paragraph.spans) {
            for (const auto& content : span.content) {
                if (const auto* text = std::get_if<std::string>(&content)) {
                    forEachUtf8Codepoint(*text, callback);
                }
            }
        }
    }
    return true;
}

bool nameContains(pugi::xml_node node, const char* needle) {
    return std::string(node.name()).find(needle) != std::string::npos;
}

void collectNodes(pugi::xml_node node, const char* needle, std::vector<pugi::xml_node>& nodes) {
    if (nameContains(node, needle)) {
        nodes.push_back(node);
    }
    for (auto child : node.children()) {
        collectNodes(child, needle, nodes);
    }
}

std::optional<uint32_t> parseCodepointAttribute(std::string value) {
    if (value.empty()) {
        return std::nullopt;
    }
    // A resource that escapes its own character reference ("&amp;#xE001;")
    // reaches us with the reference still spelled out, because pugixml only
    // expands the unescaped form. Reading it as UTF-8 would register the glyph
    // under U+0026 instead of failing, which is worse than not finding it.
    if (value.rfind("&#", 0) == 0) {
        size_t pos = 2;
        int base = 10;
        if (pos < value.size() && (value[pos] == 'x' || value[pos] == 'X')) {
            ++pos;
            base = 16;
        }
        const auto end = value.find(';', pos);
        const std::string digits =
            value.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (digits.empty()) {
            return std::nullopt;
        }
        try {
            const auto codepoint = static_cast<uint32_t>(std::stoul(digits, nullptr, base));
            return codepoint == 0 ? std::nullopt : std::optional<uint32_t>(codepoint);
        }
        catch (const std::exception&) {
            return std::nullopt;
        }
    }
    if (value.rfind("U+", 0) == 0 || value.rfind("u+", 0) == 0) {
        value = value.substr(2);
        const auto dash = value.find('-');
        if (dash != std::string::npos) {
            value = value.substr(0, dash);
        }
        if (value.empty()) {
            return std::nullopt;
        }
        try {
            return static_cast<uint32_t>(std::stoul(value, nullptr, 16));
        }
        catch (const std::exception&) {
            return std::nullopt;
        }
    }

    size_t pos = 0;
    return readUtf8Codepoint(value, pos);
}

using Point = SvgPathPoint;

struct PathParser {
    explicit PathParser(std::string_view text) : text(text) {}

    SvgPath parse() {
        char cmd = 0;
        Point current{};
        Point start{};
        Point control{};
        char lastCurve = 0;
        while (skipWs()) {
            if (isCommand(peek())) {
                cmd = get();
            }
            if (cmd == 0) {
                break;
            }

            const bool relative = std::islower(static_cast<unsigned char>(cmd)) != 0;
            switch (std::toupper(static_cast<unsigned char>(cmd))) {
            case 'M': {
                lastCurve = 0;
                auto x = number();
                auto y = number();
                if (!x || !y) {
                    return result;
                }
                current = applyRelative({ *x, *y }, current, relative);
                start = current;
                result.commands.push_back({ SvgPathCommandType::MoveTo, current });
                cmd = relative ? 'l' : 'L';
                break;
            }
            case 'L': {
                lastCurve = 0;
                while (true) {
                    auto x = number();
                    auto y = number();
                    if (!x || !y) {
                        break;
                    }
                    current = applyRelative({ *x, *y }, current, relative);
                    appendLine(current);
                }
                break;
            }
            case 'H': {
                lastCurve = 0;
                while (auto x = number()) {
                    current.x = relative ? current.x + *x : *x;
                    appendLine(current);
                }
                break;
            }
            case 'V': {
                lastCurve = 0;
                while (auto y = number()) {
                    current.y = relative ? current.y + *y : *y;
                    appendLine(current);
                }
                break;
            }
            case 'C': {
                while (true) {
                    auto x1 = number();
                    auto y1 = number();
                    auto x2 = number();
                    auto y2 = number();
                    auto x3 = number();
                    auto y3 = number();
                    if (!x1 || !y1 || !x2 || !y2 || !x3 || !y3) {
                        break;
                    }
                    const Point p1 = applyRelative({ *x1, *y1 }, current, relative);
                    const Point p2 = applyRelative({ *x2, *y2 }, current, relative);
                    const Point p3 = applyRelative({ *x3, *y3 }, current, relative);
                    appendCubic(p1, p2, p3);
                    control = p2;
                    current = p3;
                    lastCurve = 'C';
                }
                break;
            }
            case 'S': {
                while (true) {
                    auto x2 = number();
                    auto y2 = number();
                    auto x3 = number();
                    auto y3 = number();
                    if (!x2 || !y2 || !x3 || !y3) {
                        break;
                    }
                    // A smooth curve takes its leading control point from the
                    // reflection of the previous one; with no curve of the same
                    // degree before it, the current point stands in.
                    const Point p1 = lastCurve == 'C' ? reflect(control, current) : current;
                    const Point p2 = applyRelative({ *x2, *y2 }, current, relative);
                    const Point p3 = applyRelative({ *x3, *y3 }, current, relative);
                    appendCubic(p1, p2, p3);
                    control = p2;
                    current = p3;
                    lastCurve = 'C';
                }
                break;
            }
            case 'Q': {
                while (true) {
                    auto x1 = number();
                    auto y1 = number();
                    auto x2 = number();
                    auto y2 = number();
                    if (!x1 || !y1 || !x2 || !y2) {
                        break;
                    }
                    const Point p1 = applyRelative({ *x1, *y1 }, current, relative);
                    const Point p2 = applyRelative({ *x2, *y2 }, current, relative);
                    appendQuadratic(current, p1, p2);
                    control = p1;
                    current = p2;
                    lastCurve = 'Q';
                }
                break;
            }
            case 'T': {
                while (true) {
                    auto x = number();
                    auto y = number();
                    if (!x || !y) {
                        break;
                    }
                    const Point p1 = lastCurve == 'Q' ? reflect(control, current) : current;
                    const Point p2 = applyRelative({ *x, *y }, current, relative);
                    appendQuadratic(current, p1, p2);
                    control = p1;
                    current = p2;
                    lastCurve = 'Q';
                }
                break;
            }
            case 'Z':
                result.commands.push_back({ SvgPathCommandType::ClosePath, start });
                current = start;
                lastCurve = 0;
                cmd = 0;
                break;
            default:
                // Only the elliptical arc is left, and it would otherwise loop
                // forever, since number() refuses to consume a command letter.
                // What comes back is a fragment rather than a failure, so name
                // the command that stopped it instead of returning silently.
                result.unsupportedCommand = cmd;
                return result;
            }
            skipCommaWs();
        }
        return result;
    }

    bool skipWs() {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        return pos < text.size();
    }

    void skipCommaWs() {
        while (pos < text.size() && (std::isspace(static_cast<unsigned char>(text[pos])) || text[pos] == ',')) {
            ++pos;
        }
    }

    char peek() const { return pos < text.size() ? text[pos] : 0; }
    char get() { return pos < text.size() ? text[pos++] : 0; }

    static bool isCommand(char c) {
        switch (c) {
        case 'M': case 'm': case 'L': case 'l': case 'H': case 'h': case 'V': case 'v':
        case 'C': case 'c': case 'Z': case 'z':
        case 'A': case 'a': case 'Q': case 'q': case 'S': case 's': case 'T': case 't':
            return true;
        default:
            return false;
        }
    }

    std::optional<double> number() {
        skipCommaWs();
        if (pos >= text.size() || isCommand(text[pos])) {
            return std::nullopt;
        }
        const char* begin = text.data() + pos;
        char* end = nullptr;
        double value = std::strtod(begin, &end);
        if (end == begin) {
            return std::nullopt;
        }
        pos += static_cast<size_t>(end - begin);
        skipCommaWs();
        return value;
    }

    void appendLine(Point point) {
        result.commands.push_back({ SvgPathCommandType::LineTo, point });
    }

    // The start point is not stored: a command list is contiguous, so every
    // consumer already knows it as the point the previous command ended on.
    void appendCubic(Point p1, Point p2, Point p3) {
        result.commands.push_back({ SvgPathCommandType::CubicTo, p3, p1, p2 });
    }

    void appendQuadratic(Point p0, Point p1, Point p2) {
        const Point c1{ p0.x + (p1.x - p0.x) * 2.0 / 3.0,
                        p0.y + (p1.y - p0.y) * 2.0 / 3.0 };
        const Point c2{ p2.x + (p1.x - p2.x) * 2.0 / 3.0,
                        p2.y + (p1.y - p2.y) * 2.0 / 3.0 };
        appendCubic(c1, c2, p2);
    }

    static Point reflect(Point control, Point current) {
        return { 2 * current.x - control.x, 2 * current.y - control.y };
    }

    static Point applyRelative(Point p, Point base, bool relative) {
        if (relative) {
            p.x += base.x;
            p.y += base.y;
        }
        return p;
    }

    std::string_view text;
    size_t pos{};
    SvgPath result;
};

std::vector<std::vector<Point>> flattenPath(const SvgPath& path) {
    static constexpr int kFlattenSteps = 18;
    std::vector<std::vector<Point>> paths;
    Point current{};
    for (const auto& command : path.commands) {
        switch (command.type) {
        case SvgPathCommandType::MoveTo:
            current = command.point;
            paths.push_back({ current });
            break;
        case SvgPathCommandType::LineTo:
        case SvgPathCommandType::ClosePath:
            current = command.point;
            if (paths.empty()) paths.push_back({});
            paths.back().push_back(current);
            break;
        case SvgPathCommandType::CubicTo:
        {
            const Point p0 = current;
            if (paths.empty()) paths.push_back({});
            for (int i = 1; i <= kFlattenSteps; ++i) {
                const double t = static_cast<double>(i) / kFlattenSteps;
                const double mt = 1.0 - t;
                paths.back().push_back({
                    mt * mt * mt * p0.x + 3 * mt * mt * t * command.control1.x +
                        3 * mt * t * t * command.control2.x + t * t * t * command.point.x,
                    mt * mt * mt * p0.y + 3 * mt * mt * t * command.control1.y +
                        3 * mt * t * t * command.control2.y + t * t * t * command.point.y
                });
            }
            current = command.point;
            break;
        }
        }
    }
    return paths;
}

bool pointInPath(const std::vector<std::vector<Point>>& paths, double x, double y) {
    bool inside = false;
    for (const auto& path : paths) {
        if (path.size() < 3) {
            continue;
        }
        for (size_t i = 0, j = path.size() - 1; i < path.size(); j = i++) {
            const auto& pi = path[i];
            const auto& pj = path[j];
            if (((pi.y > y) != (pj.y > y)) &&
                (x < (pj.x - pi.x) * (y - pi.y) / (pj.y - pi.y + 0.000001) + pi.x)) {
                inside = !inside;
            }
        }
    }
    return inside;
}

std::vector<uint8_t> rasterizeGlyph(const DrcsGlyph& glyph, uint8_t width, uint8_t height) {
    const SvgPath parsed = parse_svg_path(glyph.path);
    if (!parsed.complete()) {
        // The fragment before the command we cannot draw is not the glyph, and
        // a pattern drawn from it is a wrong shape rather than a partial one.
        // Leaving the DRCS code undefined is the honest answer, and it is what
        // every other consumer of the shared parser does.
        subtitleDebugLog("DRCS glyph " + formatCodepoint(glyph.codepoint) +
            " not rasterized: path stops at unimplemented command '" +
            std::string(1, parsed.unsupportedCommand) + "'");
        return {};
    }
    auto paths = flattenPath(parsed);
    if (paths.empty()) {
        return {};
    }

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (const auto& path : paths) {
        for (const auto& p : path) {
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
        }
    }
    if (minX >= maxX || minY >= maxY) {
        return {};
    }

    const double pad = 1.0;
    const double scale = std::min((width - pad * 2) / (maxX - minX), (height - pad * 2) / (maxY - minY));
    const double offsetX = (width - (maxX - minX) * scale) / 2.0;
    const double offsetY = (height - (maxY - minY) * scale) / 2.0;

    std::vector<uint8_t> pixels((static_cast<size_t>(width) * height + 7) / 8, 0);
    for (uint8_t y = 0; y < height; ++y) {
        for (uint8_t x = 0; x < width; ++x) {
            const double srcX = (x + 0.5 - offsetX) / scale + minX;
            const double srcY = maxY - ((y + 0.5 - offsetY) / scale);
            if (pointInPath(paths, srcX, srcY)) {
                const size_t bit = static_cast<size_t>(y) * width + x;
                pixels[bit / 8] |= static_cast<uint8_t>(0x80 >> (bit % 8));
            }
        }
    }
    return pixels;
}

// aribTextEncode() starts from the caption default graphic sets for every call.
// The actual ARIB stream state is continuous, so reset it before appending an
// independently encoded chunk.
void appendCaptionTextStateReset(std::string& output) {
    output.push_back(static_cast<char>(B24ControlSet::ESC));
    output.push_back(static_cast<char>(0x24));
    output.push_back(static_cast<char>(0x39));
    output.push_back(static_cast<char>(B24ControlSet::ESC));
    output.push_back(static_cast<char>(0x2A));
    output.push_back(static_cast<char>(0x30));
    output.push_back(static_cast<char>(B24ControlSet::LS0));
}

} // namespace

SvgPath parse_svg_path(std::string_view path) {
    return PathParser(path).parse();
}

bool is_drcs_codepoint(uint32_t cp) {
    return (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) || (cp >= 0x100000 && cp <= 0x10FFFD);
}

std::optional<uint8_t> DrcsCodeAllocator::allocate(uint32_t codepoint) {
    auto it = codeByCodepoint_.find(codepoint);
    if (it != codeByCodepoint_.end()) {
        return it->second;
    }
    if (codeByCodepoint_.size() >= kMaxCodes) {
        return std::nullopt;
    }
    const auto code = static_cast<uint8_t>(0x21 + codeByCodepoint_.size());
    codeByCodepoint_.emplace(codepoint, code);
    return code;
}

DrcsGlyphMap parse_svg_glyph_resource(const std::string& input, bool* xmlParsed) {
    DrcsGlyphMap glyphs;
    pugi::xml_document doc;
    const bool parsed = doc.load_buffer(input.data(), input.size()).status == pugi::status_ok;
    if (xmlParsed) {
        *xmlParsed = parsed;
    }
    if (!parsed) {
        return glyphs;
    }

    std::vector<pugi::xml_node> fonts;
    collectNodes(doc, "font", fonts);
    for (auto font : fonts) {
        if (!nameContains(font, "font") || nameContains(font, "font-face")) {
            continue;
        }

        int unitsPerEm = font.attribute("horiz-adv-x").as_int(1024);
        int ascent = 880;
        int descent = -120;
        std::optional<uint32_t> faceCodepoint;
        for (auto fontFace : font.children()) {
            if (nameContains(fontFace, "font-face")) {
                unitsPerEm = fontFace.attribute("units-per-em").as_int(unitsPerEm);
                ascent = fontFace.attribute("ascent").as_int(ascent);
                descent = fontFace.attribute("descent").as_int(descent);
                faceCodepoint = parseCodepointAttribute(fontFace.attribute("unicode-range").as_string());
                break;
            }
        }

        for (auto glyph : font.children()) {
            if (!nameContains(glyph, "glyph")) {
                continue;
            }
            auto codepoint = parseCodepointAttribute(glyph.attribute("unicode").as_string());
            if (!codepoint) {
                codepoint = parseCodepointAttribute(glyph.attribute("unicode-range").as_string());
            }
            if (!codepoint) {
                // A broadcast resource routinely ships one glyph per font and
                // names the codepoint only once, on the font-face's
                // unicode-range. Dropping the glyph there loses the whole
                // resource, since the font holds nothing else.
                codepoint = faceCodepoint;
            }
            if (!codepoint) {
                continue;
            }
            std::string path = glyph.attribute("d").as_string();
            if (path.empty()) {
                continue;
            }
            glyphs[*codepoint] = DrcsGlyph{ *codepoint, unitsPerEm, ascent, descent, path };
            subtitleDebugLog("DRCS resource glyph " + formatCodepoint(*codepoint) +
                " units=" + std::to_string(unitsPerEm) +
                " ascent=" + std::to_string(ascent) +
                " descent=" + std::to_string(descent));
        }
    }

    if (glyphs.empty()) {
        std::vector<pugi::xml_node> glyphNodes;
        collectNodes(doc, "glyph", glyphNodes);
        for (auto glyph : glyphNodes) {
            auto codepoint = parseCodepointAttribute(glyph.attribute("unicode").as_string());
            if (!codepoint) {
                codepoint = parseCodepointAttribute(glyph.attribute("unicode-range").as_string());
            }
            if (codepoint && glyph.attribute("d")) {
                glyphs[*codepoint] = DrcsGlyph{ *codepoint, 1024, 880, -120, glyph.attribute("d").as_string() };
                subtitleDebugLog("DRCS resource fallback glyph " + formatCodepoint(*codepoint));
            }
        }
    }
    return glyphs;
}

bool contains_drcs_codepoint(const std::string& input) {
    bool found = false;
    const auto visit = [&](uint32_t cp) {
        if (is_drcs_codepoint(cp)) {
            subtitleDebugLog("TTML contains DRCS " + formatCodepoint(cp));
            found = true;
        }
    };

    if (forEachTtmlTextCodepoint(input, visit)) {
        return found;
    }

    forEachUtf8Codepoint(input, [&](uint32_t cp) {
        found = found || is_drcs_codepoint(cp);
    });
    return found;
}

bool has_missing_drcs_glyph(const std::string& input, const DrcsGlyphMap& glyphs) {
    bool missing = false;
    const auto visit = [&](uint32_t cp) {
        if (is_drcs_codepoint(cp) && glyphs.find(cp) == glyphs.end()) {
            subtitleDebugLog("TTML missing DRCS glyph " + formatCodepoint(cp));
            missing = true;
        }
    };

    if (forEachTtmlTextCodepoint(input, visit)) {
        return missing;
    }

    forEachUtf8Codepoint(input, [&](uint32_t cp) {
        missing = missing || (is_drcs_codepoint(cp) && glyphs.find(cp) == glyphs.end());
    });
    return missing;
}

std::vector<uint8_t> build_drcs_data_unit(const std::map<uint32_t, uint8_t>& codes, const DrcsGlyphMap& glyphs) {
    std::vector<uint8_t> data;
    data.push_back(0);
    uint8_t count = 0;
    for (const auto& [codepoint, code] : codes) {
        auto it = glyphs.find(codepoint);
        if (it == glyphs.end()) {
            continue;
        }

        constexpr uint8_t width = 36;
        constexpr uint8_t height = 36;
        auto pixels = rasterizeGlyph(it->second, width, height);
        if (pixels.empty()) {
            continue;
        }

        data.push_back(0x41);
        data.push_back(code);
        data.push_back(0x01);
        data.push_back(0x01);
        data.push_back(0x00);
        data.push_back(width);
        data.push_back(height);
        data.insert(data.end(), pixels.begin(), pixels.end());
        ++count;
    }
    data[0] = count;
    return count == 0 ? std::vector<uint8_t>{} : data;
}

std::string encode_text_with_drcs(std::string_view text, const DrcsGlyphMap& glyphs, DrcsCodeAllocator& allocator) {
    std::string output;
    size_t chunkBegin = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t cpBegin = pos;
        auto cp = readUtf8Codepoint(text, pos);
        if (!cp || glyphs.find(*cp) == glyphs.end()) {
            continue;
        }

        if (cpBegin > chunkBegin) {
            auto encoded = arib::text::encode(text.substr(chunkBegin, cpBegin - chunkBegin),
                arib::charset::EncodeMode::Caption);
            appendCaptionTextStateReset(output);
            output.append(encoded);
        }

        if (const auto code = allocator.allocate(*cp)) {
            subtitleDebugLog("DRCS emit " + formatCodepoint(*cp) + " code=0x" + formatCodepoint(*code).substr(4));
            output.push_back(static_cast<char>(B24ControlSet::ESC));
            output.push_back(static_cast<char>(0x2A));
            output.push_back(static_cast<char>(0x20));
            output.push_back(static_cast<char>(0x41));
            output.push_back(static_cast<char>(B24ControlSet::SS2));
            output.push_back(static_cast<char>(*code));
            output.push_back(static_cast<char>(B24ControlSet::LS0));
            output.push_back(static_cast<char>(B24ControlSet::ESC));
            output.push_back(static_cast<char>(0x2A));
            output.push_back(static_cast<char>(0x30));
        }
        chunkBegin = pos;
    }

    if (chunkBegin < text.size()) {
        auto encoded = arib::text::encode(text.substr(chunkBegin), arib::charset::EncodeMode::Caption);
        appendCaptionTextStateReset(output);
        output.append(encoded);
    }
    return output;
}

} // namespace ttml

} // namespace arib
