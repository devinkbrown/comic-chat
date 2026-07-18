#pragma once

#include "comicchat/cpp26.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace comicchat {

// Modern, allocation-clean port of the mIRC-style text-formatting-run model
// from v2.5-beta-1-modern/format.cpp and artifacts/inc/format.h. This file
// covers only the pure, per-message model (control-code stripping, URL
// link-run marking, and the per-line formatting reslice used by a future
// draw pass) — no CDC/CRichEditCtrl/CDWordArray, no drawing.
//
// mIRC-compatibility note: three of the seven control bytes ported here
// (chCtlBold=0x02, chCtlColor=0x03, chCtlUnderline=0x1F, format.h:5-11) are
// byte-identical to mIRC's own IRC-wire bold/color/underline control codes.
// strip_control_codes() therefore doubles as an IRC-wire formatting
// stripper for free.

// chCtlBold, chCtlColor, chCtlLink, chCtlFixedPitchFont, chCtlSymbol,
// chCtlItalic, chCtlUnderline (format.h:5-11).
inline constexpr char ctl_bold = 0x02;
inline constexpr char ctl_color = 0x03;
inline constexpr char ctl_link = 0x0C;
inline constexpr char ctl_fixed_pitch_font = 0x11;
inline constexpr char ctl_symbol = 0x12;
inline constexpr char ctl_italic = 0x16;
inline constexpr char ctl_underline = 0x1F;

// One formatting run: the format described here applies to `plain` starting
// at `offset`, up to (but not including) the next run's offset, or the end
// of the string for the last run. This is the unpacked, per-field analogue
// of the packed `MAKELONG(wFormat, wOffset)` DWORD (format.h:14-21,
// SzControlLess/format.cpp:305-309's "OOOOEEFB" comment).
//
// wFixedPitch and wSymbol (format.h:17-18) are parsed for wire fidelity
// (skip_one_format toggles internal bits for them) but intentionally not
// exposed here: the portable renderer does not yet support fixed-pitch /
// symbol font switching, and nothing downstream of Step 1-2 needs them.
struct TextRun final {
    std::size_t offset{};
    bool bold{};
    bool italic{};
    bool underline{};
    bool link{};
    std::optional<std::uint8_t> fg{};
    std::optional<std::uint8_t> bg{};
    auto operator==(const TextRun&) const -> bool = default;
};

// Result of strip_control_codes: the control-code-free text plus its
// formatting runs, sorted ascending by offset (an invariant relied on by
// mark_urls and reslice_runs_for_line).
struct StripResult final {
    std::string plain;
    std::vector<TextRun> runs;
};

// Port of SzSkipOneFormat (format.cpp:23-249) + SzControlLess
// (format.cpp:303-372): strips the mIRC-style control bytes out of `input`,
// returning the plain text and the resulting formatting runs. A run is
// recorded only when a literal character follows a format change (matching
// SzControlLess: a control sequence with nothing after it, e.g. a trailing
// `^b` at the very end of the string, produces no run — ported faithfully,
// not "fixed").
//
// DBCS note: `IsDBCSLeadByte` handling (format.cpp:275-277, 354-358) is
// intentionally dropped. Every control byte here is in 0x00-0x1F; UTF-8
// continuation/lead bytes (0x80-0xFF) can never collide with them, so a
// byte-wise scan over UTF-8 text is safe without DBCS special-casing.
[[nodiscard]] auto strip_control_codes(std::string_view input) -> StripResult;

// Port of IdentifyURLs/InsertFormat (format.cpp:1216-1247, 1049-1111):
// layers `link` runs over every URL span find_urls() (urldetect.hpp)
// discovers in `plain`, inheriting the surrounding formatting at each
// insertion point exactly as InsertFormat does. `runs` must be sorted
// ascending by offset (as produced by strip_control_codes); the result is
// too.
[[nodiscard]] auto mark_urls(const std::string& plain, std::vector<TextRun> runs) -> std::vector<TextRun>;

// Port of PullFormattingOffsets + CutFormattingArray (format.cpp:1140-1182,
// 1114-1137): given the full message's `runs`, produce the run list local to
// the line `[line_start, line_start + line_len)`. Offsets are rebased to the
// line (PullFormattingOffsets), a synthetic offset-0 run is carried over from
// whatever format was active at `line_start` if that format is non-default
// (see the wLatestFormat handling at format.cpp:1157-1179), and any run at or
// past `line_len` is dropped (CutFormattingArray). `runs` must be sorted
// ascending by offset.
[[nodiscard]] auto reslice_runs_for_line(const std::vector<TextRun>& runs, std::size_t line_start,
                                         std::size_t line_len) -> std::vector<TextRun>;

// 24-bit RGB triple for the 16-color palette below.
struct Rgb8 final {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    auto operator==(const Rgb8&) const -> bool = default;
};

// GetRBGColor's 16-entry palette (format.cpp:902-939), reproduced as data.
// This is also the classic mIRC 16-color palette byte-for-byte, another
// mIRC-compatibility bonus (see the file-level note above). Index 1 is not a
// `case` in the original switch and so falls through to the `default:
// RGB(0,0,0)` branch (format.cpp:906-907) — kept here for direct indexing.
inline constexpr std::array<Rgb8, 16> mirc_palette = {{
    {255, 255, 255}, // 0  white
    {0, 0, 0},       // 1  black (format.cpp default case; no explicit `case 1`)
    {0, 0, 128},     // 2  navy
    {0, 128, 0},     // 3  green
    {255, 0, 0},     // 4  red
    {128, 0, 0},     // 5  maroon
    {128, 0, 128},   // 6  purple
    {128, 128, 0},   // 7  olive
    {255, 255, 0},   // 8  yellow
    {0, 255, 0},     // 9  lime
    {0, 128, 128},   // 10 teal
    {0, 255, 255},   // 11 cyan
    {0, 0, 255},     // 12 blue
    {255, 0, 255},   // 13 pink / magenta
    {128, 128, 128}, // 14 gray
    {192, 192, 192}, // 15 light gray / silver
}};

// GetRBGColor(BYTE byteCode) (format.cpp:902-939): any code outside 0..15
// (or exactly 1, which was never `case`d) resolves to black, matching the
// original `default:` branch.
[[nodiscard]] constexpr auto palette_color(std::uint8_t code) noexcept -> Rgb8 {
    if (code > 15) return Rgb8{0, 0, 0};
    return mirc_palette[code];
}

} // namespace comicchat
