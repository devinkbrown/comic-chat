#pragma once

#include "comicchat/cpp26.hpp"
#include "comicchat/formatting.hpp"
#include "comicchat/layout.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace comicchat {

struct ShapedGlyph final {
    std::uint32_t index{};
    double x_advance{};
    double y_advance{};
    double x_offset{};
    double y_offset{};
};

enum class TextError { invalid_utf8, font_library, font_open, font_size, shaping };

// Font vertical metrics for a single (face, size), the FreeType-sourced analogue
// of a GDI TEXTMETRIC (balloon.cpp:606, CFontInfo::CFontInfo GetTextMetrics).
//
// HONEST METRIC-SOURCE BOUNDARY: the original derives these from GDI's
// GetTextMetrics for the installed comic face; the portable build reads
// FreeType FT_Size_Metrics for the bundled Comic Neue TTF. The GDI->FreeType
// mapping used (see build_font_metrics / TextEngine::size_metrics) is:
//   tm.tmAscent           <- FT ascender    (26.6 fixed >> 6)
//   tm.tmDescent          <- -FT descender
//   tm.tmHeight           <- ascender - descender
//   tm.tmExternalLeading  <- max(0, FT height - (ascender - descender))
// The arithmetic below is therefore self-consistent and deterministic per TTF,
// but NOT byte-identical to the Win32 GDI font mapper (render-port-spec.md
// §2.4.c). Only the derivation formulas and the bbox/line-count logic are
// Linux-provable; absolute font-pixel parity vs. GDI is MSVC/visual-only.
struct FontMetrics final {
    // tm.tmHeight + m_leading (balloon.cpp:640). Baseline-to-baseline advance
    // between wrapped lines, in the same logical units as the caller's request.
    std::int32_t line_height{};
    // m_baseAdd = nBaseAdd - tmExternalLeading (balloon.cpp:625). Extra descent
    // room added below the last line's baseline in GetBBox.
    std::int32_t base_add{};
    // m_leading = nLeading + tmExternalLeading (balloon.cpp:624).
    std::int32_t leading{};
    // m_topOffset: nLeading ? 0 : FAREAST_TOPOFFSET (balloon.cpp:635-638). Keyed
    // on the RAW (pre-leading-adjustment) nLeading argument, exactly as source.
    std::int32_t top_offset{};
    // m_continuationWidth: width of the wrap-continuation glyph "..."
    // (balloon.cpp:642). Measured with the same TextEngine that sized the font.
    std::int32_t continuation_width{};
    auto operator==(const FontMetrics&) const -> bool = default;
};

// FAREAST_TOPOFFSET (balloon.cpp:99).
inline constexpr std::int32_t far_east_top_offset = 50;

// szContinuationStr1 (balloon.cpp:109): the wrap-continuation / ellipsis glyph.
inline constexpr std::string_view continuation_ellipsis = "...";

// MAXLINES (balloon.h:7): CLabel breaks into at most this many lines.
inline constexpr std::int32_t max_label_lines = 10;

// Label justification (balloon.cpp:697): FT_LEFT_JUSTIFY vs. the default CENTER.
enum class LabelJustify { center, left };

// One wrapped line produced by break_into_lines, with its measured width in the
// caller's logical units (the rgszStarts/rgiWidths pair, balloon.cpp:363).
struct TextLine final {
    std::string text;
    std::int32_t width{};
    // Per-run formatting local to THIS line (offsets rebased to the line, as
    // produced by reslice_runs_for_line, formatting.hpp). Empty for the plain
    // break_into_lines path: a line with no runs draws as a single default-color,
    // upright, regular-weight span exactly as before the formatting feature
    // (render.cpp balloon-text-draw zero-behavior-change guarantee). Only
    // break_into_lines_formatted populates this.
    std::vector<TextRun> runs{};
    auto operator==(const TextLine&) const -> bool = default;
};

// Raw per-(face,size) metrics, mapped from FT_Size_Metrics to GDI TEXTMETRIC
// field names so the CFontInfo derivation reads identically to balloon.cpp:606.
struct SizeMetrics final {
    std::int32_t ascent{};            // tm.tmAscent
    std::int32_t descent{};           // tm.tmDescent (positive)
    std::int32_t height{};            // tm.tmHeight = ascent + descent
    std::int32_t external_leading{};  // tm.tmExternalLeading
    auto operator==(const SizeMetrics&) const -> bool = default;
};

// A width-measuring callback, `int width_in_logical_units(text)`. Injected so the
// pure wrap/ellipsis logic is testable with a synthetic (e.g. monospace) measure
// as well as with the real shaped-advance measure (measure_text_width).
using TextMeasure = std::function<std::int32_t(std::string_view)>;

// Port of ::BreakIntoLines (balloon.cpp:363): greedy word wrap to `max_width`,
// honoring explicit '\n' hard breaks, force-breaking a single word wider than
// the box, and capping at max_label_lines. Records each line's measured width.
[[nodiscard]] auto break_into_lines(const TextMeasure& measure, std::int32_t max_width,
                                    std::string_view text) -> std::vector<TextLine>;

// Formatting-aware overload of break_into_lines. Wrapping is delegated verbatim
// to the plain break_into_lines (identical line text/width/count/PRNG behavior),
// then each produced line is located back in `text` and its slice of `runs` is
// computed with reslice_runs_for_line (formatting.hpp). `runs` must be sorted
// ascending by offset and expressed in byte offsets into `text` (as produced by
// strip_control_codes + mark_urls). The result equals break_into_lines(...) with
// TextLine::runs additionally filled in; passing empty `runs` yields lines whose
// runs are all empty, i.e. it degenerates to the plain path.
//
// SPACE-COLLAPSE CAVEAT: break_into_lines collapses runs of spaces to a single
// space and trims wrap-boundary spaces, so a line's byte span is located by a
// whitespace-tolerant walk over `text`. When the source line contains a run of
// >1 consecutive spaces the collapsed line-local byte length no longer matches
// the original span exactly, so a run boundary landing inside such a gap can be
// off by the collapsed byte count. Single-space text (the overwhelmingly common
// IRC/chat case) and force-broken long tokens map exactly.
[[nodiscard]] auto break_into_lines_formatted(const TextMeasure& measure, std::int32_t max_width,
                                              std::string_view text, const std::vector<TextRun>& runs)
    -> std::vector<TextLine>;

// Widest of the produced lines (fInfo.m_iMaxWidth, balloon.cpp:694-696).
[[nodiscard]] auto widest_line_width(const std::vector<TextLine>& lines) noexcept -> std::int32_t;

// Port of the CLabel::BreakIntoLines bbox math (balloon.cpp:697-711) over the
// twips/Y-up logical Rect (top > bottom). `request` supplies Left/Right (the
// desired width) and Top; the returned Rect fills all four edges with
//   Bottom = Top - n_lines*line_height - base_add
// and the width justified around the widest line. PURE: no font, fully
// Linux-verifiable.
[[nodiscard]] auto compute_label_bbox(const Rect& request, const FontMetrics& metrics,
                                      std::int32_t n_lines, std::int32_t max_line_width,
                                      LabelJustify justify = LabelJustify::center) noexcept -> Rect;

// Port of CStarLabel's single-line DT_END_ELLIPSIS draw (balloon.cpp:1197): if
// `text` fits `max_width`, return it unchanged; otherwise return the longest
// UTF-8 prefix whose width plus `ellipsis` fits. Returns just the (clipped)
// ellipsis when not even an empty prefix leaves room, and "" when the ellipsis
// itself cannot fit.
[[nodiscard]] auto ellipsize_single_line(const TextMeasure& measure, std::int32_t max_width,
                                         std::string_view text,
                                         std::string_view ellipsis = continuation_ellipsis)
    -> std::string;

[[nodiscard]] auto normalize_utf8_nfc(std::string_view input) -> std::expected<std::string, TextError>;
[[nodiscard]] auto find_portable_comic_font() -> std::expected<std::string, TextError>;

class TextEngine final {
public:
    static auto create(std::string_view font_path) -> std::expected<std::unique_ptr<TextEngine>, TextError>;
    ~TextEngine();
    TextEngine(const TextEngine&) = delete;
    auto operator=(const TextEngine&) -> TextEngine& = delete;

    [[nodiscard]] auto shape(std::string_view utf8, double pixel_size)
        -> std::expected<std::vector<ShapedGlyph>, TextError>;
    [[nodiscard]] auto native_face() const noexcept -> void*;

    // GDI-TEXTMETRIC-shaped size metrics for this face at `pixel_size`, read from
    // FreeType FT_Size_Metrics (see FontMetrics for the mapping/limits).
    [[nodiscard]] auto size_metrics(double pixel_size) -> std::expected<SizeMetrics, TextError>;

    // Summed shaped x-advances of `utf8` at `pixel_size`, rounded to an integer —
    // the portable analogue of GetTextExtent().cx used to measure a line.
    [[nodiscard]] auto measure_width(std::string_view utf8, double pixel_size)
        -> std::expected<std::int32_t, TextError>;

private:
    class Impl;
    explicit TextEngine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Build a FontMetrics for `engine` at `pixel_size`, reproducing CFontInfo's
// derivation (balloon.cpp:606-643). `n_leading`/`n_base_add` are the per-font
// kern offsets from fonts.cpp (e.g. title (-220*reduction, 120*reduction),
// shout (0, 0)); top_offset keys on the RAW n_leading. continuation_width is
// measured by shaping continuation_ellipsis with the same engine.
[[nodiscard]] auto build_font_metrics(TextEngine& engine, double pixel_size, std::int32_t n_leading,
                                      std::int32_t n_base_add)
    -> std::expected<FontMetrics, TextError>;

// A TextMeasure bound to `engine` at `pixel_size` for break_into_lines /
// ellipsize_single_line. On a shaping failure the line measures as 0.
[[nodiscard]] auto measure_text_width(TextEngine& engine, double pixel_size) -> TextMeasure;

} // namespace comicchat
