#include "b24SubtitleConvertor.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <vector>
#include "aribUtil.h"
#include "b24Color.h"
#include "b24ControlSet.h"
#include "config.h"
#include "pugixml.hpp"

namespace {

std::mutex g_subtitleDebugLogMutex;

void subtitleDebugLog(const std::string& line) {
    if (config.subtitleDebugLogPath.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_subtitleDebugLogMutex);
    std::ofstream stream(config.subtitleDebugLogPath, std::ios::app | std::ios::binary);
    if (stream) {
        stream << line << "\n";
    }
}

std::string formatCodepoint(uint32_t cp) {
    std::ostringstream ss;
    ss << "U+" << std::uppercase << std::hex << std::setw(cp <= 0xFFFF ? 4 : 6) << std::setfill('0') << cp;
    return ss.str();
}

std::string escapeLogText(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(c);
            break;
        }
    }
    return escaped;
}

void appendNumber(std::vector<uint8_t>& output, int n) {
    if (n == 0) {
        output.push_back(0x30);
        return;
    }
    std::vector<uint8_t> temp;
    while (n > 0) {
        temp.push_back(static_cast<uint8_t>((n % 10) + 0x30));
        n /= 10;
    }
    std::reverse(temp.begin(), temp.end());
    output.insert(output.end(), temp.begin(), temp.end());
}

std::optional<uint32_t> readUtf8Codepoint(const std::string& text, size_t& pos) {
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

std::string formatCodepoints(const std::string& text) {
    std::ostringstream ss;
    bool first = true;
    size_t pos = 0;
    while (pos < text.size()) {
        auto cp = readUtf8Codepoint(text, pos);
        if (!cp) {
            continue;
        }
        if (!first) {
            ss << ' ';
        }
        ss << formatCodepoint(*cp);
        first = false;
    }
    return ss.str();
}

bool isDrcsCodepoint(uint32_t cp) {
    return (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) || (cp >= 0x100000 && cp <= 0x10FFFD);
}

template <typename Callback>
void forEachUtf8Codepoint(const std::string& text, Callback callback) {
    size_t pos = 0;
    while (pos < text.size()) {
        auto cp = readUtf8Codepoint(text, pos);
        if (cp) {
            callback(*cp);
        }
    }
}

template <typename Callback>
bool forEachTtmlTextCodepoint(const std::string& input, Callback callback) {
    try {
        const TTML ttml = TTMLPaser::parse(input);
        for (const auto& div : ttml.divTags) {
            for (const auto& p : div.pTags) {
                for (const auto& span : p.spanTags) {
                    forEachUtf8Codepoint(span.text, callback);
                }
            }
        }
        return true;
    }
    catch (...) {
        return false;
    }
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
    if (value.rfind("U+", 0) == 0 || value.rfind("u+", 0) == 0) {
        value = value.substr(2);
        const auto dash = value.find('-');
        if (dash != std::string::npos) {
            value = value.substr(0, dash);
        }
        return static_cast<uint32_t>(std::stoul(value, nullptr, 16));
    }

    size_t pos = 0;
    return readUtf8Codepoint(value, pos);
}

struct Point {
    double x{};
    double y{};
};

struct PathParser {
    explicit PathParser(std::string_view text) : text(text) {}

    std::vector<std::vector<Point>> parse() {
        char cmd = 0;
        Point current{};
        Point start{};
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
                auto x = number();
                auto y = number();
                if (!x || !y) {
                    return paths;
                }
                current = applyRelative({ *x, *y }, current, relative);
                start = current;
                paths.push_back({ current });
                cmd = relative ? 'l' : 'L';
                break;
            }
            case 'L': {
                while (true) {
                    auto x = number();
                    auto y = number();
                    if (!x || !y) {
                        break;
                    }
                    current = applyRelative({ *x, *y }, current, relative);
                    ensurePath().push_back(current);
                }
                break;
            }
            case 'H': {
                while (auto x = number()) {
                    current.x = relative ? current.x + *x : *x;
                    ensurePath().push_back(current);
                }
                break;
            }
            case 'V': {
                while (auto y = number()) {
                    current.y = relative ? current.y + *y : *y;
                    ensurePath().push_back(current);
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
                    const Point p0 = current;
                    const Point p1 = applyRelative({ *x1, *y1 }, current, relative);
                    const Point p2 = applyRelative({ *x2, *y2 }, current, relative);
                    const Point p3 = applyRelative({ *x3, *y3 }, current, relative);
                    for (int i = 1; i <= 18; ++i) {
                        const double t = i / 18.0;
                        const double mt = 1.0 - t;
                        ensurePath().push_back({
                            mt * mt * mt * p0.x + 3 * mt * mt * t * p1.x + 3 * mt * t * t * p2.x + t * t * t * p3.x,
                            mt * mt * mt * p0.y + 3 * mt * mt * t * p1.y + 3 * mt * t * t * p2.y + t * t * t * p3.y
                        });
                    }
                    current = p3;
                }
                break;
            }
            case 'Z':
                ensurePath().push_back(start);
                cmd = 0;
                break;
            default:
                return paths;
            }
            skipCommaWs();
        }
        return paths;
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
        case 'M': case 'm': case 'L': case 'l': case 'H': case 'h': case 'V': case 'v': case 'C': case 'c': case 'Z': case 'z':
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

    static Point applyRelative(Point p, Point base, bool relative) {
        if (relative) {
            p.x += base.x;
            p.y += base.y;
        }
        return p;
    }

    std::vector<Point>& ensurePath() {
        if (paths.empty()) {
            paths.push_back({});
        }
        return paths.back();
    }

    std::string_view text;
    size_t pos{};
    std::vector<std::vector<Point>> paths;
};

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

std::vector<uint8_t> rasterizeGlyph(const B24DrcsGlyph& glyph, uint8_t width, uint8_t height) {
    auto paths = PathParser(glyph.path).parse();
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

    std::vector<uint8_t> pixels((width * height + 7) / 8, 0);
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

std::vector<uint8_t> buildDrcsDataUnit(const std::map<uint32_t, uint8_t>& codeByCodepoint, const std::unordered_map<uint32_t, B24DrcsGlyph>& glyphs) {
    std::vector<uint8_t> data;
    data.push_back(0);
    uint8_t count = 0;
    for (const auto& [codepoint, code] : codeByCodepoint) {
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

void appendCaptionTextStateReset(std::vector<uint8_t>& output) {
    // aribEncode() starts from the caption default graphic sets for every call.
    // The actual ARIB stream state is continuous, so reset it before appending
    // an independently encoded span/chunk.
    output.push_back(B24ControlSet::ESC);
    output.push_back(0x24);
    output.push_back(0x39);
    output.push_back(B24ControlSet::ESC);
    output.push_back(0x2A);
    output.push_back(0x30);
    output.push_back(B24ControlSet::LS0);
}

std::vector<uint8_t> encodeTextWithDrcs(const std::string& text, const std::unordered_map<uint32_t, B24DrcsGlyph>& glyphs, std::map<uint32_t, uint8_t>& codeByCodepoint) {
    std::vector<uint8_t> output;
    size_t chunkBegin = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t cpBegin = pos;
        auto cp = readUtf8Codepoint(text, pos);
        if (!cp || glyphs.find(*cp) == glyphs.end()) {
            continue;
        }

        if (cpBegin > chunkBegin) {
            auto encoded = aribEncode(text.substr(chunkBegin, cpBegin - chunkBegin), true);
            appendCaptionTextStateReset(output);
            output.insert(output.end(), encoded.begin(), encoded.end());
        }

        if (codeByCodepoint.size() < 94) {
            auto [it, inserted] = codeByCodepoint.emplace(*cp, static_cast<uint8_t>(0x21 + codeByCodepoint.size()));
            subtitleDebugLog("DRCS emit " + formatCodepoint(*cp) + " code=0x" + formatCodepoint(it->second).substr(4));
            output.push_back(B24ControlSet::ESC);
            output.push_back(0x2A);
            output.push_back(0x20);
            output.push_back(0x41);
            output.push_back(B24ControlSet::SS2);
            output.push_back(it->second);
            output.push_back(B24ControlSet::LS0);
            output.push_back(B24ControlSet::ESC);
            output.push_back(0x2A);
            output.push_back(0x30);
        }
        chunkBegin = pos;
    }

    if (chunkBegin < text.size()) {
        auto encoded = aribEncode(text.substr(chunkBegin), true);
        appendCaptionTextStateReset(output);
        output.insert(output.end(), encoded.begin(), encoded.end());
    }
    return output;
}

} // namespace

bool B24SubtitleConvertor::convert(const std::string& input, std::list<B24SubtitleOutput>& output) {
    return convert(input, {}, output);
}

bool B24SubtitleConvertor::convert(const std::string& input, const std::unordered_map<uint32_t, B24DrcsGlyph>& drcsGlyphs, std::list<B24SubtitleOutput>& output) {
    const TTML ttml = TTMLPaser::parse(input);
    subtitleDebugLog("TTML convert glyphs=" + std::to_string(drcsGlyphs.size()));

    uint8_t lastTextColorPalette = 0;
    uint8_t lastTextColorIndex = 7;
    uint8_t lastBackgroundColorPalette = 0;
    uint8_t lastBackgroundColorIndex = 8;
    uint8_t characterSize = B24ControlSet::NSZ;
    double fontHeight = 0;

    for (const auto& div : ttml.divTags) {
        B24::CaptionStatementData captionStatementData;
        std::map<uint32_t, uint8_t> drcsCodes;

        std::vector<uint8_t> unitDataByte;
        unitDataByte.push_back(B24ControlSet::CS);

        unitDataByte.push_back(B24ControlSet::CSI);
        unitDataByte.push_back(0x37);
        unitDataByte.push_back(B24ControlSet::SP);
        unitDataByte.push_back(B24ControlSet::SWF);

        for (const auto& p : div.pTags) {
            if (p.spanTags.empty()) {
                continue;
            }

            if (p.region.extent.has_value()) {
                unitDataByte.push_back(B24ControlSet::CSI);
                appendNumber(unitDataByte, static_cast<uint32_t>(p.region.extent->first.getValue<TTMLCssValueLength>().value * 960 / 3840));
                unitDataByte.push_back(0x3B);
                appendNumber(unitDataByte, static_cast<uint32_t>(p.region.extent->second.getValue<TTMLCssValueLength>().value * 540 / 2160));
                unitDataByte.push_back(B24ControlSet::SP);
                unitDataByte.push_back(B24ControlSet::SDF);
            }

            const auto firstSpan = p.spanTags.begin();
            if (firstSpan != p.spanTags.end() && firstSpan->style.fontSize) {
                TTMLCssValueLength first = firstSpan->style.fontSize->first.getValue<TTMLCssValueLength>();
                TTMLCssValueLength second = firstSpan->style.fontSize->second.getValue<TTMLCssValueLength>();

                if (first.value == 144 && second.value == 144) {
                    if (characterSize != B24ControlSet::NSZ) {
                        unitDataByte.push_back(B24ControlSet::NSZ);
                        characterSize = B24ControlSet::NSZ;
                    }
                    fontHeight = 240;
                }
                else if (first.value == 72 && second.value == 144) {
                    if (characterSize != B24ControlSet::MSZ) {
                        unitDataByte.push_back(B24ControlSet::MSZ);
                        characterSize = B24ControlSet::MSZ;
                    }
                    fontHeight = 240;
                }
                else if (first.value == 72 && second.value == 72) {
                    if (characterSize != B24ControlSet::SSZ) {
                        unitDataByte.push_back(B24ControlSet::SSZ);
                        characterSize = B24ControlSet::SSZ;
                    }
                    fontHeight = 120;
                }
            }

            if (p.region.origin.has_value()) {
                double offsetY = 0;
                if (p.spanTags.begin()->style.lineHeight.has_value() && p.spanTags.begin()->style.fontSize) {
                    double lineHeight = p.spanTags.begin()->style.lineHeight->getValue<TTMLCssValueLength>().value;
                    if (p.region.extent.has_value()) {
                        offsetY = (lineHeight - fontHeight) / 2;
                    }
                }

                unitDataByte.push_back(B24ControlSet::CSI);
                appendNumber(unitDataByte, static_cast<uint32_t>(p.region.origin->first.getValue<TTMLCssValueLength>().value * 960 / 3840));
                unitDataByte.push_back(0x3B);
                appendNumber(unitDataByte, static_cast<uint32_t>((p.region.origin->second.getValue<TTMLCssValueLength>().value + offsetY) * 540 / 2160));
                unitDataByte.push_back(B24ControlSet::SP);
                unitDataByte.push_back(B24ControlSet::SDP);
            }

            unitDataByte.push_back(B24ControlSet::APS);
            unitDataByte.push_back(0x40);
            unitDataByte.push_back(0x40);

            for (const auto& span : p.spanTags) {
                if (span.style.fontSize) {
                    TTMLCssValueLength first = span.style.fontSize->first.getValue<TTMLCssValueLength>();
                    TTMLCssValueLength second = span.style.fontSize->second.getValue<TTMLCssValueLength>();

                    if (first.value == 144 && second.value == 144) {
                        if (characterSize != B24ControlSet::NSZ) {
                            unitDataByte.push_back(B24ControlSet::NSZ);
                            characterSize = B24ControlSet::NSZ;
                        }
                    }
                    else if (first.value == 72 && second.value == 144) {
                        if (characterSize != B24ControlSet::MSZ) {
                            unitDataByte.push_back(B24ControlSet::MSZ);
                            characterSize = B24ControlSet::MSZ;
                        }
                    }
                    else if (first.value == 72 && second.value == 72) {
                        if (characterSize != B24ControlSet::SSZ) {
                            unitDataByte.push_back(B24ControlSet::SSZ);
                            characterSize = B24ControlSet::SSZ;
                        }
                    }
                }

                if (span.style.backgroundColor.has_value()) {
                    TTMLCssValueColor color = span.style.backgroundColor->getValue<TTMLCssValueColor>();
                    auto closetColor = findClosestColor(ColorRGBA{ color.r, color.g, color.b, color.a });

                    if (lastBackgroundColorPalette != closetColor.first || lastBackgroundColorIndex != closetColor.second) {
                        unitDataByte.push_back(B24ControlSet::COL);
                        unitDataByte.push_back(0x20);
                        unitDataByte.push_back(0x40 | closetColor.first);
                        unitDataByte.push_back(B24ControlSet::COL);
                        unitDataByte.push_back(0x50 | closetColor.second);

                        lastBackgroundColorPalette = closetColor.first;
                        lastBackgroundColorIndex = closetColor.second;
                    }
                }

                if (span.style.color.has_value()) {
                    TTMLCssValueColor color = span.style.color->getValue<TTMLCssValueColor>();
                    auto closetColor = findClosestColor(ColorRGBA{ color.r, color.g, color.b, color.a });

                    if (lastTextColorPalette != closetColor.first || lastTextColorIndex != closetColor.second) {
                        if (closetColor.first == 0 && closetColor.second >= 0 && closetColor.second <= 7) {
                            unitDataByte.push_back(B24ControlSet::COL);
                            unitDataByte.push_back(0x20);
                            unitDataByte.push_back(0x40 | 0);
                            unitDataByte.push_back(B24ControlSet::BKF + closetColor.second);
                        }
                        else {
                            unitDataByte.push_back(B24ControlSet::COL);
                            unitDataByte.push_back(0x20);
                            unitDataByte.push_back(0x40 | closetColor.first);
                            unitDataByte.push_back(B24ControlSet::COL);
                            unitDataByte.push_back(0x40 | closetColor.second);
                        }

                        lastTextColorPalette = closetColor.first;
                        lastTextColorIndex = closetColor.second;
                    }
                }

                std::string fsLog = "none";
                if (span.style.fontSize) {
                    auto w = (int)span.style.fontSize->first.getValue<TTMLCssValueLength>().value;
                    auto h = (int)span.style.fontSize->second.getValue<TTMLCssValueLength>().value;
                    fsLog = std::to_string(w) + "x" + std::to_string(h);
                }
                subtitleDebugLog("TTML span text=\"" + escapeLogText(span.text) + "\" fontSize=" + fsLog + " cps=" + formatCodepoints(span.text));
                auto encoded = encodeTextWithDrcs(span.text, drcsGlyphs, drcsCodes);
                unitDataByte.insert(unitDataByte.end(), encoded.begin(), encoded.end());
            }
        }

        if (!drcsCodes.empty()) {
            auto drcsDataUnit = buildDrcsDataUnit(drcsCodes, drcsGlyphs);
            if (!drcsDataUnit.empty()) {
                captionStatementData.dataUnits.push_back({ drcsDataUnit, B24::DataUnitParameter::DRCS1Byte });
            }
        }
        captionStatementData.dataUnits.push_back({ unitDataByte });

        if (div.begin && div.end) {
            std::vector<uint8_t> unitDataByte;

            uint64_t duration = (*div.end - *div.begin) / 100;
            while (duration > 0) {
                uint8_t value = static_cast<uint8_t>(std::min(duration, static_cast<uint64_t>(0x3F)));
                unitDataByte.push_back(B24ControlSet::TIME);
                unitDataByte.push_back(0x20);
                unitDataByte.push_back(0x40 | value);
                unitDataByte.push_back(0x0C);
                duration -= value;
            }

            captionStatementData.dataUnits.push_back({ unitDataByte });
        }

        B24::DataGroup dataGroup;
        dataGroup.setGroupData(captionStatementData);

        std::vector<uint8_t> packedPesData;
        B24::PESData pesData(dataGroup);
        pesData.SetPESType(B24::PESData::PESType::Synchronized);
        pesData.pack(packedPesData);

        output.push_back({ packedPesData , div.begin });
    }

    if (output.size() == 0) {
        B24::CaptionStatementData captionStatementData;
        {
            std::vector<uint8_t> unitDataByte;
            unitDataByte.push_back(B24ControlSet::CS);
            captionStatementData.dataUnits.push_back({ unitDataByte });
        }
        B24::DataGroup dataGroup;
        dataGroup.setGroupData(captionStatementData);

        std::vector<uint8_t> packedPesData;
        B24::PESData pesData(dataGroup);
        pesData.SetPESType(B24::PESData::PESType::Synchronized);
        pesData.pack(packedPesData);

        output.push_back({ packedPesData , 0 });
    }

    return true;
}

std::unordered_map<uint32_t, B24DrcsGlyph> B24SubtitleConvertor::parseSvgGlyphResource(const std::string& input) {
    std::unordered_map<uint32_t, B24DrcsGlyph> glyphs;
    pugi::xml_document doc;
    if (doc.load_buffer(input.data(), input.size()).status != pugi::status_ok) {
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
        for (auto fontFace : font.children()) {
            if (nameContains(fontFace, "font-face")) {
                unitsPerEm = fontFace.attribute("units-per-em").as_int(unitsPerEm);
                ascent = fontFace.attribute("ascent").as_int(ascent);
                descent = fontFace.attribute("descent").as_int(descent);
                break;
            }
        }

        for (auto glyph : font.children()) {
            if (!nameContains(glyph, "glyph")) {
                continue;
            }
            auto codepoint = parseCodepointAttribute(glyph.attribute("unicode").as_string());
            if (!codepoint) {
                continue;
            }
            std::string path = glyph.attribute("d").as_string();
            if (path.empty()) {
                continue;
            }
            glyphs[*codepoint] = B24DrcsGlyph{ *codepoint, unitsPerEm, ascent, descent, path };
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
                glyphs[*codepoint] = B24DrcsGlyph{ *codepoint, 1024, 880, -120, glyph.attribute("d").as_string() };
                subtitleDebugLog("DRCS resource fallback glyph " + formatCodepoint(*codepoint));
            }
        }
    }
    return glyphs;
}

bool B24SubtitleConvertor::containsDrcsCodepoint(const std::string& input) {
    bool found = false;
    if (forEachTtmlTextCodepoint(input, [&](uint32_t cp) {
        if (isDrcsCodepoint(cp)) {
            subtitleDebugLog("TTML contains DRCS " + formatCodepoint(cp));
        }
        found = found || isDrcsCodepoint(cp);
    })) {
        return found;
    }

    forEachUtf8Codepoint(input, [&](uint32_t cp) {
        found = found || isDrcsCodepoint(cp);
    });
    return found;
}

bool B24SubtitleConvertor::hasMissingDrcsGlyph(const std::string& input, const std::unordered_map<uint32_t, B24DrcsGlyph>& drcsGlyphs) {
    bool missing = false;
    if (forEachTtmlTextCodepoint(input, [&](uint32_t cp) {
        if (isDrcsCodepoint(cp) && drcsGlyphs.find(cp) == drcsGlyphs.end()) {
            subtitleDebugLog("TTML missing DRCS glyph " + formatCodepoint(cp));
        }
        missing = missing || (isDrcsCodepoint(cp) && drcsGlyphs.find(cp) == drcsGlyphs.end());
    })) {
        return missing;
    }

    forEachUtf8Codepoint(input, [&](uint32_t cp) {
        missing = missing || (isDrcsCodepoint(cp) && drcsGlyphs.find(cp) == drcsGlyphs.end());
    });
    return missing;
}
