#include "ttml/b24_converter.h"
#include "ttml/drcs.h"
#include "aribTextEncoder.h"
#include "b24Color.h"
#include "b24ControlSet.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <vector>
#include <cmath>

namespace arib {

namespace ttml {

namespace {

constexpr double kB24LayoutWidth = 960.0;

constexpr double sourceLayoutWidth(MmtTlv::SubtitleResolution resolution) {
    switch (resolution) {
    case MmtTlv::SubtitleResolution::Resolution2K:
        return 1920.0;
    case MmtTlv::SubtitleResolution::Resolution4K:
        return 3840.0;
    case MmtTlv::SubtitleResolution::Resolution8K:
        return 7680.0;
    }

    return 3840.0;
}

struct StyledRun {
    std::string_view text;
    const StyleProperties* style;
};

struct Line {
    std::vector<StyledRun> runs;
};

struct RegionBlock {
    const resolved::Region* region;
    std::vector<Line> lines;
};

struct Page {
    std::vector<RegionBlock> blocks;
};

struct SpanCue {
    const resolved::Paragraph* paragraph;
    const resolved::Span* span;
    const resolved::Region* region;
    size_t order;
};

enum class SpanRegionMode {
    None,
    StyleOnly,
    Positioned,
};

struct CollectResult {
    std::vector<SpanCue> cues;
    std::optional<B24ConvertError> error;
};

struct TimelineEvent {
    std::chrono::milliseconds time;
    std::vector<const SpanCue*> starts;
    std::vector<const SpanCue*> ends;
};

struct RenderAction {
    std::optional<std::chrono::milliseconds> begin;
    uint64_t delay;
    bool clear;
    Page page;
};

uint64_t to_deciseconds(std::chrono::milliseconds time) {
    return static_cast<uint64_t>(time.count() / 100);
}

std::vector<std::string> split_by_null(std::string_view data) {
    std::vector<std::string> output;
    output.reserve(static_cast<size_t>(std::ranges::count(data, '\0')));
    size_t begin = 0;

    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\0') {
            output.emplace_back(data.substr(begin, i - begin));
            begin = i + 1;
        }
    }

    if (begin < data.size()) {
        output.emplace_back(data.substr(begin));
    }
    return output;
}

// Vertical writing (tts:writingMode="tbrl") is laid out in the horizontal
// writing format with every character placed on its own (ACPS), top to bottom
// and right to left, since caption renderers do not lay out the vertical
// writing format of ARIB STD-B24. That needs a region to place the columns in.
bool is_vertical(const RegionBlock& block) {
    const auto& style = block.region->style;
    return style.writing_mode == StyleWritingModeValue::Tbrl && style.origin && style.extent;
}

// Punctuation drawn differently in vertical text, as its vertical presentation
// form (Unicode Vertical Forms and CJK Compatibility Forms). These have no
// caption code, so they are drawn from the font as DRCS.
uint32_t vertical_form(uint32_t cp) {
    switch (cp) {
    case 0xFF0C: return 0xFE10; // FULLWIDTH COMMA
    case 0x3001: return 0xFE11; // IDEOGRAPHIC COMMA
    case 0x3002: return 0xFE12; // IDEOGRAPHIC FULL STOP
    case 0xFF1A: return 0xFE13; // FULLWIDTH COLON
    case 0xFF1B: return 0xFE14; // FULLWIDTH SEMICOLON
    case 0x3016: return 0xFE17; // LEFT WHITE LENTICULAR BRACKET
    case 0x3017: return 0xFE18; // RIGHT WHITE LENTICULAR BRACKET
    case 0x2026: return 0xFE19; // HORIZONTAL ELLIPSIS
    case 0x2025: return 0xFE30; // TWO DOT LEADER
    case 0x30FC: return 0xFE31; // PROLONGED SOUND MARK
    case 0x2014: return 0xFE31; // EM DASH
    case 0x2015: return 0xFE31; // HORIZONTAL BAR
    case 0x2013: return 0xFE32; // EN DASH
    case 0xFF3F: return 0xFE33; // FULLWIDTH LOW LINE
    case 0xFF08: return 0xFE35; // FULLWIDTH LEFT PARENTHESIS
    case 0xFF09: return 0xFE36; // FULLWIDTH RIGHT PARENTHESIS
    case 0xFF5B: return 0xFE37; // FULLWIDTH LEFT CURLY BRACKET
    case 0xFF5D: return 0xFE38; // FULLWIDTH RIGHT CURLY BRACKET
    case 0x3014: return 0xFE39; // LEFT TORTOISE SHELL BRACKET
    case 0x3015: return 0xFE3A; // RIGHT TORTOISE SHELL BRACKET
    case 0x3010: return 0xFE3B; // LEFT BLACK LENTICULAR BRACKET
    case 0x3011: return 0xFE3C; // RIGHT BLACK LENTICULAR BRACKET
    case 0x300A: return 0xFE3D; // LEFT DOUBLE ANGLE BRACKET
    case 0x300B: return 0xFE3E; // RIGHT DOUBLE ANGLE BRACKET
    case 0x3008: return 0xFE3F; // LEFT ANGLE BRACKET
    case 0x3009: return 0xFE40; // RIGHT ANGLE BRACKET
    case 0x300C: return 0xFE41; // LEFT CORNER BRACKET
    case 0x300D: return 0xFE42; // RIGHT CORNER BRACKET
    case 0x300E: return 0xFE43; // LEFT WHITE CORNER BRACKET
    case 0x300F: return 0xFE44; // RIGHT WHITE CORNER BRACKET
    case 0xFF3B: return 0xFE47; // FULLWIDTH LEFT SQUARE BRACKET
    case 0xFF3D: return 0xFE48; // FULLWIDTH RIGHT SQUARE BRACKET
    case 0xFF5E: return 0x2240; // FULLWIDTH TILDE (no vertical form; a wave standing upright)
    case 0x301C: return 0x2240; // WAVE DASH
    case 0xFF0D: return 0xFF5C; // FULLWIDTH HYPHEN-MINUS
    case 0xFF1D: return 0x2016; // FULLWIDTH EQUALS SIGN
    case 0x2010: return 0xFE32; // HYPHEN
    case 0x2016: return 0xFF1D; // DOUBLE VERTICAL LINE
    case 0xFF5C: return 0x2015; // FULLWIDTH VERTICAL LINE
    case 0x25B6: return 0x25BC; // BLACK RIGHT-POINTING TRIANGLE
    case 0x25C0: return 0x25B2; // BLACK LEFT-POINTING TRIANGLE
    default: return cp;
    }
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Small kana sit in the upper right of their cell in vertical text (ARIB
// TR-B39 Table 8-4), where the horizontal glyph has them in the lower left.
bool is_small_kana(uint32_t cp) {
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

struct VerticalCharacter {
    std::string text;
    bool small_kana = false;
};

// The characters of a run of vertical text, each in its vertical form. A
// malformed byte is kept as it is, as the horizontal path would.
std::vector<VerticalCharacter> vertical_characters(std::string_view text) {
    std::vector<VerticalCharacter> characters;
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<uint8_t>(text[i]);
        const size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
        if (i + length > text.size() || length == 1) {
            characters.push_back({ std::string(text.substr(i, 1)) });
            ++i;
            continue;
        }
        uint32_t cp = lead & (0xFF >> (length + 1));
        for (size_t k = 1; k < length; ++k) {
            cp = (cp << 6) | (static_cast<uint8_t>(text[i + k]) & 0x3F);
        }
        VerticalCharacter character;
        append_utf8(character.text, vertical_form(cp));
        character.small_kana = is_small_kana(cp);
        characters.push_back(std::move(character));
        i += length;
    }
    return characters;
}

bool span_has_text(const resolved::Span& span) {
    return std::ranges::any_of(span.content, [](const auto& content) {
        const auto* text = std::get_if<std::string>(&content);
        return text && !text->empty();
    });
}

void append_span(RegionBlock& block, const resolved::Span& span) {
    if (block.lines.empty()) {
        block.lines.emplace_back();
    }

    for (const auto& content : span.content) {
        if (const auto* text = std::get_if<std::string>(&content)) {
            if (!text->empty()) {
                block.lines.back().runs.push_back(StyledRun{
                    .text = *text,
                    .style = &span.style,
                });
            }
        }
        else {
            block.lines.emplace_back();
        }
    }
}

class CueCollector {
public:
    CueCollector(const resolved::Document& document, SyncMode mode)
        : document_(document), mode_(mode) {
    }

    CollectResult run() const {
        CollectResult output;
        size_t order = 0;

        if (!document_.division) {
            return output;
        }

        for (const auto& paragraph : document_.division->paragraphs) {
            // ARIB-TTML has no initial font size, so every span that draws text
            // must resolve one. A span holding only a line break draws nothing
            // and contributes no styling, so requiring a size there would drop
            // the whole paragraph over a <br/> between two spans.
            const bool has_missing_font_size = std::ranges::any_of(paragraph.spans,
                [](const resolved::Span& span) {
                    return !span.style.font_size && span_has_text(span);
                }
            );
            if (has_missing_font_size) {
                continue;
            }

            // ARIB-TTML requires all spans to use the same region reference.
            std::optional<SpanRegionMode> paragraph_region_mode;
            bool has_mixed_region_modes = false;
            for (const auto& span : paragraph.spans) {
                const bool has_origin = span.region && span.region->style.origin.has_value();
                const bool has_extent = span.region && span.region->style.extent.has_value();
                if (has_origin != has_extent) {
                    // The existing validation below reports an incomplete origin/extent pair.
                    continue;
                }

                const auto region_mode = !span.region ? SpanRegionMode::None :
                    (has_origin ? SpanRegionMode::Positioned : SpanRegionMode::StyleOnly);
                if (!paragraph_region_mode) {
                    paragraph_region_mode = region_mode;
                }
                else if (*paragraph_region_mode != region_mode) {
                    has_mixed_region_modes = true;
                    break;
                }
            }
            if (has_mixed_region_modes) {
                continue;
            }

            for (const auto& span : paragraph.spans) {
                if (mode_ == SyncMode::Sync && span.timing.begin && span.timing.end &&
                    *span.timing.end <= *span.timing.begin) {
                    continue;
                }

                // A line break renders even though it carries no text.
                const bool has_content = std::ranges::any_of(span.content,
                    [](const auto& content) {
                        const auto* text = std::get_if<std::string>(&content);
                        return text ? !text->empty() : true;
                    }
                );
                if (!has_content) {
                    continue;
                }

                const resolved::Region* paragraph_region = nullptr;
                if (paragraph.region) {
                    const bool has_origin = paragraph.region->style.origin.has_value();
                    const bool has_extent = paragraph.region->style.extent.has_value();
                    if (has_origin != has_extent) {
                        output.error = B24ConvertError{
                            "paragraph region must specify both tts:origin and tts:extent"
                        };
                        return output;
                    }
                    if (has_origin) {
                        paragraph_region = &*paragraph.region;
                    }
                }

                const resolved::Region* region = nullptr;
                if (span.region) {
                    const bool has_origin = span.region->style.origin.has_value();
                    const bool has_extent = span.region->style.extent.has_value();
                    if (has_origin != has_extent) {
                        output.error = B24ConvertError{
                            "span region must specify both tts:origin and tts:extent"
                        };
                        return output;
                    }
                    if (has_origin) {
                        region = &*span.region;
                    }
                }

                if (!region) {
                    // A span region without geometry is style-only and continues in the paragraph region.
                    region = paragraph_region;
                }

                if (!region) {
                    output.error = B24ConvertError{ "span has no effective region" };
                    return output;
                }

                output.cues.push_back(SpanCue{
                    .paragraph = &paragraph,
                    .span = &span,
                    .region = region,
                    .order = order++,
                });
            }
        }

        return output;
    }

private:
    const resolved::Document& document_;
    SyncMode mode_;

};

class TimelineBuilder {
public:
    explicit TimelineBuilder(const std::vector<SpanCue>& cues)
        : cues_(cues) {
    }

    std::vector<TimelineEvent> run() const {
        std::map<std::chrono::milliseconds, TimelineEvent> events;

        for (const auto& cue : cues_) {
            const auto begin = cue.span->timing.begin.value_or(std::chrono::milliseconds{});
            auto& start_event = events[begin];
            start_event.time = begin;
            start_event.starts.push_back(&cue);

            if (cue.span->timing.end) {
                const auto end = *cue.span->timing.end;
                auto& end_event = events[end];
                end_event.time = end;
                end_event.ends.push_back(&cue);
            }
        }

        std::vector<TimelineEvent> output;
        output.reserve(events.size());
        for (auto& entry : events) {
            output.push_back(std::move(entry.second));
        }
        return output;
    }

private:
    const std::vector<SpanCue>& cues_;
};

class Encoder {
public:
    Encoder(const std::vector<SpanCue>& cues, SyncMode mode, MmtTlv::SubtitleResolution resolution,
            const DrcsGlyphMap& drcs_glyphs)
        : cues_(cues), mode_(mode), layoutScale_(kB24LayoutWidth / sourceLayoutWidth(resolution)),
          drcs_glyphs_(drcs_glyphs) {
    }

    B24ConvertResult run() {
        if (mode_ == SyncMode::Async) {
            return encode_async();
        }
        else {
			return encode_sync();
        }
    }

private:
    static Page build_page(std::span<const SpanCue* const> active_cues) {
        Page page;
        const resolved::Paragraph* current_paragraph = nullptr;
        const resolved::Region* current_region = nullptr;
        RegionBlock* block = nullptr;

        for (const auto* cue : active_cues) {
            if (!block || cue->paragraph != current_paragraph || cue->region != current_region) {
                page.blocks.push_back(RegionBlock{
                    .region = cue->region,
                    .lines = { Line{} },
                });
                block = &page.blocks.back();
                current_paragraph = cue->paragraph;
                current_region = cue->region;
            }

            append_span(*block, *cue->span);
        }

        return page;
    }

    B24ConvertResult encode_async() {
        std::vector<const SpanCue*> cues;
        cues.reserve(cues_.size());
        for (const auto& cue : cues_) {
            cues.push_back(&cue);
        }

        const auto page = build_page(cues);
        auto encoded_runs = encode_text(page);
        size_t encoded_run_index = 0;
        append_initial_format();
        encode_page(page, encoded_runs, encoded_run_index);

        return {
            .outputs = { B24ConvertOutput{
                .data = std::move(output_),
                .drcs = take_drcs_data_unit(),
                .begin = {},
                .clear = false,
            } },
            .error = {},
        };
    }

    B24ConvertResult encode_sync() {
        const auto actions = build_render_actions();
        if (actions.empty()) {
            return {
                .outputs = { B24ConvertOutput{
                    .data = {},
                    .drcs = {},
                    .begin = {},
                    .clear = true,
                } },
                .error = {},
            };
        }

        B24ConvertResult result;
        result.outputs.reserve(actions.size());
        for (size_t i = 0; i < actions.size(); ++i) {
            const auto& action = actions[i];
            const bool is_last_clear = i + 1 == actions.size()
                && action.clear
                && action.page.blocks.empty()
                && !result.outputs.empty();
            if (is_last_clear) {
                output_ = std::move(result.outputs.back().data);
                append_time(action.delay);
                append_clear();
                result.outputs.back().data = std::move(output_);
                continue;
            }

            auto encoded_runs = encode_text(action.page);
            size_t encoded_run_index = 0;
            output_.clear();
            reset_state();
            append_initial_format();

            encode_page(action.page, encoded_runs, encoded_run_index);

            result.outputs.push_back(B24ConvertOutput{
                .data = std::move(output_),
                .drcs = take_drcs_data_unit(),
                .begin = action.begin,
                .clear = action.clear,
            });
        }

        return result;
    }

    std::vector<RenderAction> build_render_actions() const {
        std::vector<RenderAction> actions;
        std::vector<const SpanCue*> active_cues;
        std::chrono::milliseconds current_time{};
        bool first_event = true;

        active_cues.reserve(cues_.size());

        const auto events = TimelineBuilder{cues_}.run();
        actions.reserve(events.size());
        for (const auto& event : events) {
            const auto delay = first_event ? 0 : to_deciseconds(event.time) - to_deciseconds(current_time);

            for (const auto* cue : event.ends) {
                std::erase(active_cues, cue);
            }
            for (const auto* cue : event.starts) {
                const auto position = std::ranges::lower_bound(active_cues, cue->order, {},
                    [](const SpanCue* active_cue) {
                        return active_cue->order;
                    }
                );
                active_cues.insert(position, cue);
            }

            const bool starts_without_time = std::ranges::any_of(event.starts,
                [](const SpanCue* cue) {
                    return !cue->span->timing.begin;
                }
            );
            actions.push_back(RenderAction{
                .begin = first_event && starts_without_time ? std::nullopt : std::optional{event.time},
                .delay = delay,
                .clear = !first_event,
                .page = build_page(active_cues),
            });

            current_time = event.time;
            first_event = false;
        }
        return actions;
    }

    // One encoded chunk per run, or per character in vertical text, which
    // places every character on its own.
    static void collect_text(const Page& page, std::string& output) {
        for (const auto& block : page.blocks) {
            const bool vertical = is_vertical(block);
            for (const auto& line : block.lines) {
                for (const auto& run : line.runs) {
                    if (vertical) {
                        for (const auto& character : vertical_characters(run.text)) {
                            output.append(character.text);
                            output.push_back('\0');
                        }
                        continue;
                    }
                    output.append(run.text);
                    output.push_back('\0');
                }
            }
        }
    }

    std::vector<std::string> encode_text(const Page& page) {
        std::string text;
        collect_text(page, text);

        drcs_codes_.clear();
        drcs_font_patterns_.clear();
        // A character the caption has no code for is drawn from a font and sent
        // as a DRCS pattern, so the same path is taken for a page that holds one
        // even when the subtitle shipped no glyph resource of its own.
        const bool needsDrcs =
            !drcs_glyphs_.empty() || contains_unencodable_text(text);
        if (needsDrcs) {
            // Runs are encoded in one call so the charset state carries across
            // them, then split back apart on the NUL separators. A DRCS glyph
            // cannot go through the text encoder, so a page that uses one falls
            // back to chunk-wise encoding, which re-establishes the graphic
            // sets around every DRCS designation.
            return split_by_null(
                encode_text_with_drcs(text, drcs_glyphs_, drcs_codes_, &drcs_font_patterns_));
        }

        return split_by_null(arib::text::encode(text, arib::charset::EncodeMode::Caption));
    }

    std::vector<uint8_t> take_drcs_data_unit() {
        if (drcs_codes_.empty()) {
            return {};
        }
        return build_drcs_data_unit(drcs_codes_.codes(), drcs_glyphs_, drcs_font_patterns_);
    }

    void encode_page(const Page& page, const std::vector<std::string>& encoded_runs, size_t& encoded_run_index) {
        for (const auto& block : page.blocks) {
            if (is_vertical(block)) {
                encode_vertical_block(block, encoded_runs, encoded_run_index);
                continue;
            }
            if (block.region->style.extent) {
                append_extent(*block.region->style.extent);
            }

            if (block.region->style.origin) {
                append_origin(*block.region->style.origin);
            }

            bool aps = false;
            for (size_t line_index = 0; line_index < block.lines.size(); ++line_index) {
                const auto& line = block.lines[line_index];
                for (const auto& run : line.runs) {
                    const auto& style = *run.style;
                    // MSZ halves the whole character cell horizontally, spacing
                    // included, and SSZ halves it both ways (ARIB STD-B24), so
                    // the spacings are sent doubled for those sizes.
                    const bool middle_size = style.font_size && style.font_size->x == 72 && style.font_size->y == 144;
                    const bool small_size = style.font_size && style.font_size->x == 72 && style.font_size->y == 72;
                    const double horizontal_scale = middle_size || small_size ? 0.5 : 1.0;
                    const double vertical_scale = small_size ? 0.5 : 1.0;
                    if (style.letter_spacing) {
                        const auto spacing = static_cast<uint32_t>(std::max(0.0, std::round(
                                *style.letter_spacing * layoutScale_ / horizontal_scale)));
                        set_character_spacing(spacing);
                    }

                    if (style.line_height && style.font_size) {
                        const double font_height = style.font_size->y;
                        const double line_height = std::holds_alternative<NormalLineHeight>(*style.line_height)
                                ? font_height * 1.2
                                : std::get<double>(*style.line_height);
                        const auto spacing = static_cast<uint32_t>(std::max(0.0, std::round(
                                (line_height - font_height) * layoutScale_ / vertical_scale)));
                        set_line_spacing(spacing);
                    }

                    apply_run_style(style);

                    if (!aps) {
                        append_aps(0, 0);
                        aps = true;
                    }
                    const auto& encoded = encoded_runs.at(encoded_run_index++);
                    output_.insert(output_.end(), encoded.begin(), encoded.end());
                }
                if (line_index + 1 < block.lines.size()) {
                    output_.push_back(B24ControlSet::APR);
                }
            }
        }
    }

    // Vertical text: lines are columns from the right edge of the region and
    // characters go down them. Each character is placed with ACPS, whose
    // position is the lower left of its character cell; SHS is 0 so the cell
    // is as wide as the glyph, and SVS is the letter spacing so the cell is
    // as tall as one step down the column (the glyph is drawn SVS/2 into it,
    // the character display zone of ARIB TR-B39 8.3.4).
    //
    // Two half-width characters on their own (a run of exactly two, such as
    // a two-digit number) share one cell side by side (tate-chu-yoko); other
    // half-width characters stand upright one per cell. Small kana move to the
    // upper right of their cell by an eighth of the glyph.
    void encode_vertical_block(const RegionBlock& block, const std::vector<std::string>& encoded_runs, size_t& encoded_run_index) {
        const auto& origin = *block.region->style.origin;
        const auto& extent = *block.region->style.extent;
        append_extent(extent);
        append_origin(origin);

        const double top = origin.y * layoutScale_;
        double column_right = (origin.x + extent.x) * layoutScale_;
        for (const auto& line : block.lines) {
            // The column is as wide as the widest line height on it.
            double column_width = 0;
            for (const auto& run : line.runs) {
                const auto& style = *run.style;
                const double font_width = style.font_size ? style.font_size->x : 144.0;
                double width = font_width * 1.2;
                if (style.line_height && !std::holds_alternative<NormalLineHeight>(*style.line_height)) {
                    width = std::get<double>(*style.line_height);
                }
                column_width = std::max(column_width, width * layoutScale_);
            }
            const double column_left = column_right - column_width;

            double y = top;
            for (const auto& run : line.runs) {
                const auto& style = *run.style;
                apply_run_style(style);
                const bool small_size = character_size_ == B24ControlSet::SSZ;
                const bool half_width = small_size || character_size_ == B24ControlSet::MSZ;
                const double horizontal_scale = half_width ? 0.5 : 1.0;
                const double vertical_scale = small_size ? 0.5 : 1.0;
                const double letter_spacing = style.letter_spacing ? std::max(0.0, *style.letter_spacing * layoutScale_) : 0.0;
                set_character_spacing(0);
                set_line_spacing(static_cast<uint32_t>(std::round(letter_spacing / vertical_scale)));

                const double glyph_width = character_composition_width_ * horizontal_scale;
                const double glyph_height = character_composition_height_ * vertical_scale;
                const double step = (character_composition_height_ + line_spacing_) * vertical_scale;
                const double x = column_left + (column_width - glyph_width) / 2;
                const auto characters = vertical_characters(run.text);
                if (character_size_ == B24ControlSet::MSZ && characters.size() == 2) {
                    y += step;
                    for (size_t i = 0; i < 2; ++i) {
                        const double offset = (i == 0 ? -0.5 : 0.5) * glyph_width;
                        append_acps(std::lround(x + offset), std::lround(y));
                        const auto& encoded = encoded_runs.at(encoded_run_index++);
                        output_.insert(output_.end(), encoded.begin(), encoded.end());
                    }
                    continue;
                }
                for (const auto& character : characters) {
                    y += step;
                    const double dx = character.small_kana ? glyph_width / 8 : 0.0;
                    const double dy = character.small_kana ? -glyph_height / 8 : 0.0;
                    append_acps(std::lround(x + dx), std::lround(y + dy));
                    const auto& encoded = encoded_runs.at(encoded_run_index++);
                    output_.insert(output_.end(), encoded.begin(), encoded.end());
                }
            }
            column_right = column_left;
        }
    }

    // The character size and decoration of a run, the same in both writing
    // directions.
    void apply_run_style(const StyleProperties& style) {
        if (style.font_size) {
            const double font_width = style.font_size->x;
            const double font_height = style.font_size->y;
            uint32_t composition_width = static_cast<uint32_t>(std::round(144 * layoutScale_));
            uint32_t composition_height = static_cast<uint32_t>(std::round(144 * layoutScale_));
            uint8_t character_size = B24ControlSet::NSZ;
            if (font_width == 72 && font_height == 144) {
                character_size = B24ControlSet::MSZ;
            }
            else if (font_width == 72 && font_height == 72) {
                character_size = B24ControlSet::SSZ;
            }
            else {
                composition_width = static_cast<uint32_t>(std::max(1.0, std::round(font_width * layoutScale_)));
                composition_height = static_cast<uint32_t>(std::max(1.0, std::round(font_height * layoutScale_)));
            }
            set_character_composition(composition_width, composition_height);
            set_character_size(character_size);
        }

        if (!style.text_outline || std::holds_alternative<None>(*style.text_outline)) {
            set_ornament_off();
        }
        else {
            const auto& text_outline = std::get<TextOutline>(*style.text_outline);
            const auto color = text_outline.color.value_or(
                style.color.value_or(StyleColorValue{ { 255, 255, 255, 255 } }));
            const auto [palette, index] = findClosestColor(ColorRGBA{
                color.r(), color.g(), color.b(), color.a()});
            set_ornament(palette, index);
        }

        if (style.color) {
            set_text_color(*style.color);
        }

        if (style.background_color) {
            set_background_color(*style.background_color);
        }

        const bool underline = style.text_decoration == StyleTextDecorationValue::Underline;
        set_underline(underline);

        // HLC: bit 0 bottom, 1 right, 2 top, 3 left.
        uint8_t enclosure = 0;
        if (style.border_bottom.value_or(false)) enclosure |= 0x01;
        if (style.border_right.value_or(false)) enclosure |= 0x02;
        if (style.border_top.value_or(false)) enclosure |= 0x04;
        if (style.border_left.value_or(false)) enclosure |= 0x08;
        set_enclosure(enclosure);

        set_flashing(style.flashing.value_or(false));

        uint8_t font = 0;
        if (style.font_weight == StyleFontWeightValue::Bold) {
            font |= 0x01;
        }
        if (style.font_style == StyleFontStyleValue::Italic) {
            font |= 0x02;
        }
        set_font(font);
    }

    void reset_state() {
        last_text_color_palette_ = 0;
        last_text_color_index_ = 7;
        last_background_color_palette_ = 0;
        last_background_color_index_ = 8;
        character_size_ = B24ControlSet::NSZ;
        character_composition_width_ = 36;
        character_composition_height_ = 36;
        line_spacing_ = 24;
        character_spacing_ = 4;
        underline_ = false;
        enclosure_ = 0;
        flashing_ = false;
        font_ = 0;
        ornament_enabled_ = false;
        ornament_palette_ = 0;
        ornament_index_ = 0;
    }

    void append_clear() {
        output_.push_back(B24ControlSet::CS);
        reset_state();
    }

    void append_initial_format() {
        output_.push_back(B24ControlSet::CSI);
        output_.push_back(0x37);
        output_.push_back(B24ControlSet::SP);
        output_.push_back(B24ControlSet::SWF);
    }

    void append_number(int n) {
        n = std::max(n, 0);

        std::array<char, std::numeric_limits<int>::digits10 + 1> buffer;
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), n);
        for (auto current = buffer.data(); current != result.ptr; ++current) {
            output_.push_back(static_cast<uint8_t>(*current));
        }
    }

    void append_extent(const LengthPair& extent) {
        output_.push_back(B24ControlSet::CSI);
        append_number(static_cast<uint32_t>(extent.x * layoutScale_));
        output_.push_back(0x3B);
        append_number(static_cast<uint32_t>(extent.y * layoutScale_));
        output_.push_back(B24ControlSet::SP);
        output_.push_back(B24ControlSet::SDF);
    }

    void append_origin(const LengthPair& origin) {
        output_.push_back(B24ControlSet::CSI);
        append_number(static_cast<int>(std::lround(origin.x * layoutScale_)));
        output_.push_back(0x3B);
        append_number(static_cast<int>(std::lround(origin.y * layoutScale_)));
        output_.push_back(B24ControlSet::SP);
        output_.push_back(B24ControlSet::SDP);
    }

    void append_acps(long x, long y) {
        output_.push_back(B24ControlSet::CSI);
        append_number(static_cast<int>(x));
        output_.push_back(0x3B);
        append_number(static_cast<int>(y));
        output_.push_back(B24ControlSet::SP);
        output_.push_back(B24ControlSet::ACPS);
    }

    void append_aps(uint8_t row, uint8_t column) {
        output_.push_back(B24ControlSet::APS);
        output_.push_back(static_cast<uint8_t>(0x40 | row));
        output_.push_back(static_cast<uint8_t>(0x40 | column));
    }

    void set_character_composition(uint32_t width, uint32_t height) {
        if (character_composition_width_ == width &&
            character_composition_height_ == height) {
            return;
        }

        output_.push_back(B24ControlSet::CSI);
        append_number(width);
        output_.push_back(0x3B);
        append_number(height);
        output_.push_back(B24ControlSet::SP);
        output_.push_back(B24ControlSet::SSM);

        character_composition_width_ = width;
        character_composition_height_ = height;
    }

    void set_character_size(uint8_t size) {
        if (character_size_ == size) {
            return;
        }

        output_.push_back(size);
        character_size_ = size;
    }

    void append_spacing(uint32_t spacing, uint8_t control) {
        output_.push_back(B24ControlSet::CSI);
        append_number(spacing);
        output_.push_back(B24ControlSet::SP);
        output_.push_back(control);
    }

    void set_line_spacing(uint32_t spacing) {
        if (line_spacing_ == spacing) {
            return;
        }

        append_spacing(spacing, B24ControlSet::SVS);
        line_spacing_ = spacing;
    }

    void set_character_spacing(uint32_t spacing) {
        if (character_spacing_ == spacing) {
            return;
        }

        append_spacing(spacing, B24ControlSet::SHS);
        character_spacing_ = spacing;
    }

    void set_underline(bool underline) {
        if (underline_ == underline) {
            return;
        }

        output_.push_back(underline ? B24ControlSet::STL : B24ControlSet::SPL);
        underline_ = underline;
    }

    void set_enclosure(uint8_t sides) {
        if (enclosure_ == sides) {
            return;
        }
        output_.push_back(B24ControlSet::HLC);
        output_.push_back(static_cast<uint8_t>(0x40 | sides));
        enclosure_ = sides;
    }

    // FLC 0x40 starts normal-phase flashing, 0x4F stops it.
    void set_flashing(bool flashing) {
        if (flashing_ == flashing) {
            return;
        }
        output_.push_back(B24ControlSet::FLC);
        output_.push_back(flashing ? 0x40 : 0x4F);
        flashing_ = flashing;
    }

    void set_text_color(const StyleColorValue& color) {
        const auto [palette, index] = findClosestColor(ColorRGBA{
            color.r(), color.g(), color.b(), color.a()
        });
        if (last_text_color_palette_ == palette && last_text_color_index_ == index) {
            return;
        }

        output_.push_back(B24ControlSet::COL);
        output_.push_back(0x20);
        output_.push_back(static_cast<uint8_t>(0x40 | palette));
        if (index <= 7) {
            output_.push_back(static_cast<uint8_t>(B24ControlSet::BKF + index));
        }
        else {
            output_.push_back(B24ControlSet::COL);
            output_.push_back(static_cast<uint8_t>(0x40 | index));
        }

        last_text_color_palette_ = palette;
        last_text_color_index_ = index;
    }

    void set_background_color(const StyleColorValue& color) {
        const auto [palette, index] = findClosestColor(ColorRGBA{
            color.r(), color.g(), color.b(), color.a()
        });
        if (last_background_color_palette_ == palette &&
            last_background_color_index_ == index) {
            return;
        }

        output_.push_back(B24ControlSet::COL);
        output_.push_back(0x20);
        output_.push_back(static_cast<uint8_t>(0x40 | palette));
        output_.push_back(B24ControlSet::COL);
        output_.push_back(static_cast<uint8_t>(0x50 | index));

        last_background_color_palette_ = palette;
        last_background_color_index_ = index;
    }

    void append_two_digits(uint8_t value) {
        output_.push_back(static_cast<uint8_t>(0x30 + value / 10));
        output_.push_back(static_cast<uint8_t>(0x30 + value % 10));
    }

    void set_ornament(uint8_t palette, uint8_t index) {
        if (ornament_enabled_ && ornament_palette_ == palette && ornament_index_ == index) {
            return;
        }

        output_.push_back(B24ControlSet::CSI);
        output_.push_back(0x31);
        output_.push_back(0x3B);
        append_two_digits(palette);
        append_two_digits(index);
        output_.push_back(B24ControlSet::SP);
        output_.push_back(B24ControlSet::ORN);

        ornament_enabled_ = true;
        ornament_palette_ = palette;
        ornament_index_ = index;
    }

    void set_ornament_off() {
        if (!ornament_enabled_) {
            return;
        }

        output_.push_back(B24ControlSet::CSI);
        output_.push_back(0x30);
        output_.push_back(B24ControlSet::SP);
        output_.push_back(B24ControlSet::ORN);

        ornament_enabled_ = false;
    }

    void set_font(uint8_t font) {
        if (font_ == font) {
            return;
        }

        output_.push_back(B24ControlSet::CSI);
        output_.push_back(0x30 + font);
        output_.push_back(B24ControlSet::SP);
        output_.push_back(B24ControlSet::MDF);

        font_ = font;
    }

    void append_time(std::uint64_t duration) {
        while (duration > 0) {
            const auto value = static_cast<std::uint8_t>(std::min<std::uint64_t>(duration, 0x3F));
            output_.push_back(B24ControlSet::TIME);
            output_.push_back(0x20);
            output_.push_back(0x40 | value);
            duration -= value;
        }
    }

private:
    const std::vector<SpanCue>& cues_;
    SyncMode mode_;
    double layoutScale_;
    const DrcsGlyphMap& drcs_glyphs_;
    DrcsCodeAllocator drcs_codes_;
    DrcsPatternMap drcs_font_patterns_;
    std::vector<std::uint8_t> output_;
    uint8_t last_text_color_palette_{0};
    uint8_t last_text_color_index_{7};
    uint8_t last_background_color_palette_{0};
    uint8_t last_background_color_index_{8};
    uint8_t character_size_{B24ControlSet::NSZ};
    uint32_t character_composition_width_{36};
    uint32_t character_composition_height_{36};
    uint32_t line_spacing_{24};
    uint32_t character_spacing_{4};
    bool underline_{false};
    uint8_t enclosure_{0};
    bool flashing_{false};
    uint8_t font_{0};
    bool ornament_enabled_{false};
    uint8_t ornament_palette_{0};
    uint8_t ornament_index_{0};

};

} // namespace

B24ConvertResult convert_to_b24(const resolved::Document& document, SyncMode mode,
                                MmtTlv::SubtitleResolution resolution, const DrcsGlyphMap& drcs_glyphs) {
    auto collected = CueCollector{document, mode}.run();
    if (collected.error) {
        return {
            .outputs = {},
            .error = std::move(collected.error),
        };
    }

    return Encoder{collected.cues, mode, resolution, drcs_glyphs}.run();
}

} // namespace ttml

} // namespace arib
