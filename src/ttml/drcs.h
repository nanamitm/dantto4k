#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arib {

namespace ttml {

// A glyph carried by an ARIB-TTML subtitle resource MFU (an SVG font). The
// broadcast uses private-use codepoints for characters that have no Unicode
// equivalent, and ships the outlines out-of-band; they are rasterized into B24
// DRCS patterns so ARIB-capable players can render them.
struct DrcsGlyph {
    uint32_t codepoint{};
    int unitsPerEm{1024};
    int ascent{880};
    int descent{-120};
    std::string path;
};

using DrcsGlyphMap = std::unordered_map<uint32_t, DrcsGlyph>;

struct SvgPathPoint {
    double x{};
    double y{};
};

enum class SvgPathCommandType {
    MoveTo,
    LineTo,
    CubicTo,
    ClosePath,
};

// Normalized SVG path command. H/V are represented as LineTo, while Q/T/S
// are converted to CubicTo after relative coordinates and reflected control
// points have been resolved. ClosePath.point is the subpath start.
struct SvgPathCommand {
    SvgPathCommandType type{SvgPathCommandType::MoveTo};
    SvgPathPoint point{};
    SvgPathPoint control1{};
    SvgPathPoint control2{};
};

struct SvgPath {
    std::vector<SvgPathCommand> commands;
    char unsupportedCommand{};

    [[nodiscard]] bool complete() const { return unsupportedCommand == 0; }
};

// Parses and normalizes the SVG path dialect used by ARIB-TTML glyph
// resources. A partial command list can be returned with unsupportedCommand
// set; consumers must not mistake that for a complete glyph.
[[nodiscard]] SvgPath parse_svg_path(std::string_view path);

[[nodiscard]] bool is_drcs_codepoint(uint32_t codepoint);

// Parses an SVG font resource into the glyphs it defines. Returns an empty map
// when the payload is not a font this converter understands.
[[nodiscard]] DrcsGlyphMap parse_svg_glyph_resource(const std::string& input);

// True when the TTML document renders at least one private-use codepoint.
[[nodiscard]] bool contains_drcs_codepoint(const std::string& input);

// True when the TTML document renders a private-use codepoint whose glyph has
// not been received yet, i.e. the caption must wait for its resource MFU.
[[nodiscard]] bool has_missing_drcs_glyph(const std::string& input, const DrcsGlyphMap& glyphs);

// Assigns 1-byte DRCS codes (0x21..0x7E) to codepoints as they are encountered.
// A caption statement can carry at most 94 of them.
class DrcsCodeAllocator {
public:
    static constexpr size_t kMaxCodes = 94;

    void clear() { codeByCodepoint_.clear(); }
    [[nodiscard]] bool empty() const { return codeByCodepoint_.empty(); }
    [[nodiscard]] const std::map<uint32_t, uint8_t>& codes() const { return codeByCodepoint_; }

    // Returns the code for `codepoint`, allocating one if the table is not full.
    [[nodiscard]] std::optional<uint8_t> allocate(uint32_t codepoint);

private:
    std::map<uint32_t, uint8_t> codeByCodepoint_;
};

// Builds the DRCS_1byte data unit payload for the allocated codes. Returns an
// empty vector when no glyph could be rasterized.
[[nodiscard]] std::vector<uint8_t> build_drcs_data_unit(const std::map<uint32_t, uint8_t>& codes,
                                                        const DrcsGlyphMap& glyphs);

// Encodes caption text that mixes ordinary characters with DRCS codepoints.
// Ordinary runs go through arib::text::encode() in Caption mode; DRCS
// codepoints become a designation + SS2 + code sequence. NUL separators in
// `text` are preserved so the caller can still split the result per run.
[[nodiscard]] std::string encode_text_with_drcs(std::string_view text,
                                                const DrcsGlyphMap& glyphs,
                                                DrcsCodeAllocator& allocator);

} // namespace ttml

} // namespace arib
