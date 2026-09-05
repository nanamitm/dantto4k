#pragma once

#include <string>
#include <string_view>

namespace arib::charset {

enum class EncodeMode {
    Text,
    Caption,
};

std::string encode(std::string_view input, EncodeMode mode = EncodeMode::Text);
std::string encode(std::u8string_view input, EncodeMode mode = EncodeMode::Text);

// True when a caption can carry this character. ARIB overlays its own additional
// Kanji and symbols on rows 85-94 of the JIS compatible Kanji set that captions
// designate, so a character JIS X 0213 only has in those rows has no code to be
// sent in; encode() drops it. A caller that must show it anyway has to send it
// some other way - as a DRCS pattern, for instance.
[[nodiscard]] bool canEncodeCaption(char32_t codepoint);

} // namespace arib::charset
