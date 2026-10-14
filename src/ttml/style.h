#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace arib {

namespace ttml {

struct None {};

struct StyleColorValue {
    std::array<uint8_t, 4> rgba;

    constexpr uint8_t r() const { return rgba[0]; }
    constexpr uint8_t g() const { return rgba[1]; }
    constexpr uint8_t b() const { return rgba[2]; }
    constexpr uint8_t a() const { return rgba[3]; }

    friend constexpr bool operator==(const StyleColorValue&, const StyleColorValue&) = default;
};

struct LengthPair {
    double x{};
    double y{};
};

enum class SingleLengthMode {
    Reject,
    Repeat,
};

enum class StyleFontStyleValue {
    Normal,
    Italic
};

enum class StyleFontWeightValue {
    Normal,
    Bold
};

struct NormalLineHeight {};
using StyleLineHeightValue = std::variant<NormalLineHeight, double>;

enum class StyleTextDecorationValue {
    None,
    Underline
};

struct TextOutline {
    std::optional<StyleColorValue> color;
    double border_width{};
    double blur_width{};
};

using StyleTextOutlineValue = std::variant<None, TextOutline>;

enum class StyleWritingModeValue {
    Lrtb,
    Tbrl
};

struct StyleProperties {
    std::optional<StyleColorValue> background_color;
    std::optional<StyleColorValue> color;
    std::optional<LengthPair> extent;
    std::optional<std::string> font_family;
    std::optional<LengthPair> font_size;
    std::optional<StyleFontStyleValue> font_style;
    std::optional<StyleFontWeightValue> font_weight;
    std::optional<StyleLineHeightValue> line_height;
    std::optional<LengthPair> origin;
    std::optional<StyleTextDecorationValue> text_decoration;
    std::optional<StyleTextOutlineValue> text_outline;
    std::optional<StyleWritingModeValue> writing_mode;
    std::optional<double> letter_spacing;
    std::optional<std::string> ruby;
    // Character enclosure per side (arib-tt:border, arib-tt:border-top, ...).
    // Only whether a side is drawn survives: B24 draws enclosures in the
    // character color with its own line width.
    std::optional<bool> border_top;
    std::optional<bool> border_right;
    std::optional<bool> border_bottom;
    std::optional<bool> border_left;
    // arib-tt:animation: the animation name as written, and whether that
    // animation is a flashing one (opacity keyframes only, ARIB TR-B39 8.5.7.1).
    std::optional<std::string> animation;
    std::optional<bool> flashing;
};

[[nodiscard]] std::optional<StyleColorValue> parse_color(std::string_view text);
[[nodiscard]] std::optional<double> parse_length(std::string_view value);
[[nodiscard]] std::optional<LengthPair> parse_length_pair(std::string_view length_string, SingleLengthMode single_length_mode);
[[nodiscard]] std::optional<StyleTextOutlineValue> parse_text_outline(std::string_view value_string);
// "<style> <width> <color>" of arib-tt:border*: true for "solid", false for
// "none", nullopt when malformed.
[[nodiscard]] std::optional<bool> parse_border(std::string_view value);

} // namespace ttml

} // namespace arib
