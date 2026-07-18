#include "comicchat/formatting.hpp"

#include "comicchat/urldetect.hpp"

#include <cctype>

namespace comicchat {

namespace {

// wBold..wLink (format.h:14-21): the packed WORD bit layout SzSkipOneFormat
// operates on. Bits 0-3 hold the background color index, bits 4-7 the
// foreground color index, bits 8-15 the boolean effect flags.
constexpr std::uint16_t w_bold = 0x0100;
constexpr std::uint16_t w_italic = 0x0200;
constexpr std::uint16_t w_underline = 0x0400;
constexpr std::uint16_t w_fixed_pitch = 0x0800;
constexpr std::uint16_t w_symbol = 0x1000;
constexpr std::uint16_t w_foreground = 0x2000;
constexpr std::uint16_t w_background = 0x4000;

[[nodiscard]] constexpr auto is_format_control_char(char c) noexcept -> bool {
    // format.cpp:321-328 (SzControlLess' switch). chCtlLink is deliberately
    // absent: SzSkipOneFormat/SzControlLess never reference it (it is only
    // ever produced by SzControlFull, format.cpp:546, tagged there as
    // "i don't think this is ever used..."); wLink is instead applied
    // programmatically by mark_urls, matching IdentifyURLs.
    switch (c) {
    case ctl_color:
    case ctl_bold:
    case ctl_italic:
    case ctl_fixed_pitch_font:
    case ctl_underline:
    case ctl_symbol:
        return true;
    default:
        return false;
    }
}

// Sentinel-safe byte peek: returns '\0' past the end, matching the original
// C string's implicit null terminator so digit/comma lookahead checks fail
// closed instead of reading out of bounds.
[[nodiscard]] constexpr auto peek(std::string_view s, std::size_t i) noexcept -> char {
    return i < s.size() ? s[i] : '\0';
}

[[nodiscard]] constexpr auto is_ascii_digit(char c) noexcept -> bool {
    return c >= '0' && c <= '9';
}

// Port of SzSkipOneFormat's chCtlColor case (format.cpp:35-189): parses
// `^c`, `^cW`, `^cWX`, `^cW,X`, `^cWX,Y`, `^cWX,YZ`, `^c,X`, `^c,XY`, and the
// "lonely ^c" reset forms, folding the two-digit values mod 16 exactly as
// upstream. `pos` indexes the chCtlColor byte itself; returns the index of
// the last byte consumed (mirrors the original's post-decrement `szRead`,
// i.e. one less than SzSkipOneFormat's `return szRead + 1`, since the caller
// here adds the `+1`).
[[nodiscard]] constexpr auto skip_color(std::string_view input, std::size_t pos, std::uint16_t& fmt)
    -> std::size_t {
    std::size_t read = pos;

    if (!is_ascii_digit(peek(input, read + 1))) {
        if (peek(input, read + 1) == ',') {
            ++read; // at ','
            if (is_ascii_digit(peek(input, read + 1))) {
                // ^c,X? -> background only, foreground omitted/defaulted.
                fmt &= static_cast<std::uint16_t>(~w_foreground);
                fmt |= w_background;
                fmt &= 0xFF00;
                ++read; // at first bg digit
                if (is_ascii_digit(peek(input, read + 1))) {
                    fmt |= static_cast<std::uint16_t>(
                        ((input[read] - '0') * 10 + (peek(input, read + 1) - '0')) % 16);
                    ++read;
                } else {
                    fmt |= static_cast<std::uint16_t>(input[read] - '0');
                }
            } else {
                // ^c,R : lonely "^c," -> reset both colors.
                fmt &= static_cast<std::uint16_t>(~w_foreground);
                fmt &= static_cast<std::uint16_t>(~w_background);
                fmt &= 0xFF00;
                --read; // give the comma back to the caller as plain text
            }
        } else {
            // lonely ^c -> reset both colors.
            fmt &= static_cast<std::uint16_t>(~w_foreground);
            fmt &= static_cast<std::uint16_t>(~w_background);
            fmt &= 0xFF00;
        }
        return read;
    }

    ++read; // at first fg digit
    fmt |= w_foreground;
    fmt &= 0xFF0F;
    if (is_ascii_digit(peek(input, read + 1))) {
        fmt |= static_cast<std::uint16_t>(
            (((input[read] - '0') * 10 + (peek(input, read + 1) - '0')) % 16) << 4);
        ++read;
        if (peek(input, read + 1) == ',') {
            ++read;
            if (is_ascii_digit(peek(input, read + 1))) {
                fmt |= w_background;
                fmt &= 0xFFF0;
                ++read;
                if (is_ascii_digit(peek(input, read + 1))) {
                    fmt |= static_cast<std::uint16_t>(
                        ((input[read] - '0') * 10 + (peek(input, read + 1) - '0')) % 16);
                    ++read;
                } else {
                    fmt |= static_cast<std::uint16_t>(input[read] - '0');
                }
            } else {
                --read; // give the comma back
            }
        }
    } else {
        if (peek(input, read + 1) == ',') {
            fmt |= static_cast<std::uint16_t>((input[read] - '0') << 4);
            ++read;
            if (is_ascii_digit(peek(input, read + 1))) {
                fmt |= w_background;
                fmt &= 0xFFF0;
                ++read;
                if (is_ascii_digit(peek(input, read + 1))) {
                    fmt |= static_cast<std::uint16_t>(
                        ((input[read] - '0') * 10 + (peek(input, read + 1) - '0')) % 16);
                    ++read;
                } else {
                    fmt |= static_cast<std::uint16_t>(input[read] - '0');
                }
            } else {
                --read; // give the comma back
            }
        } else {
            fmt |= static_cast<std::uint16_t>((input[read] - '0') << 4);
        }
    }
    return read;
}

// Port of SzSkipOneFormat (format.cpp:23-249) in full: dispatches on the
// control byte at `pos` and returns the index just past the whole consumed
// sequence (mirrors `return szRead + 1;`, format.cpp:248).
[[nodiscard]] constexpr auto skip_one_format(std::string_view input, std::size_t pos, std::uint16_t& fmt)
    -> std::size_t {
    switch (input[pos]) {
    case ctl_color:
        return skip_color(input, pos, fmt) + 1;
    case ctl_bold:
        fmt ^= w_bold; // format.cpp:191-199
        return pos + 1;
    case ctl_italic:
        fmt ^= w_italic; // format.cpp:201-209
        return pos + 1;
    case ctl_fixed_pitch_font:
        fmt ^= w_fixed_pitch; // format.cpp:211-219
        return pos + 1;
    case ctl_underline:
        fmt ^= w_underline; // format.cpp:221-229
        return pos + 1;
    case ctl_symbol:
        fmt ^= w_symbol; // format.cpp:231-239
        return pos + 1;
    default:
        return pos + 1; // unreachable: caller only dispatches known control bytes
    }
}

[[nodiscard]] constexpr auto to_text_run(std::uint16_t fmt, std::size_t offset) noexcept -> TextRun {
    TextRun run{};
    run.offset = offset;
    run.bold = (fmt & w_bold) != 0;
    run.italic = (fmt & w_italic) != 0;
    run.underline = (fmt & w_underline) != 0;
    if (fmt & w_foreground) run.fg = static_cast<std::uint8_t>((fmt >> 4) & 0x0F);
    if (fmt & w_background) run.bg = static_cast<std::uint8_t>(fmt & 0x0F);
    return run;
}

// "if (wFormat)" truthy check (format.cpp:1147, 1157, 1164, 1173), narrowed
// to the fields TextRun actually carries (fixed-pitch/symbol are not
// tracked; see formatting.hpp's TextRun doc comment).
[[nodiscard]] constexpr auto has_any_format(const TextRun& run) noexcept -> bool {
    return run.bold || run.italic || run.underline || run.link || run.fg.has_value() || run.bg.has_value();
}

// Generic port of InsertFormat (format.cpp:1049-1111), specialized to a
// single boolean field via `get`/`set`. IdentifyURLs (format.cpp:1216-1247)
// only ever calls InsertFormat with wLink, so mark_urls is the sole caller.
template <typename Get, typename Set>
void insert_bool_format(std::vector<TextRun>& runs, bool add_format, std::size_t offset, Get get, Set set) {
    if (runs.empty()) {
        // format.cpp:1051-1061 (array starts empty: no inherited format to
        // fold in).
        TextRun run{};
        run.offset = offset;
        set(run, add_format);
        runs.push_back(run);
        return;
    }

    // format.cpp:1068-1073: first index whose offset >= `offset`.
    std::size_t i = 0;
    while (i < runs.size() && runs[i].offset < offset) ++i;
    const bool exact = (i < runs.size() && runs[i].offset == offset);

    // format.cpp:1075-1081: inherit the format in effect just before
    // `offset` (or the all-default format if inserting at the very front).
    TextRun tmp = exact ? runs[i] : (i == 0 ? TextRun{} : runs[i - 1]);
    tmp.offset = offset;
    set(tmp, add_format);

    if (exact) {
        runs[i] = tmp; // format.cpp:1088-1089
    } else {
        runs.insert(runs.begin() + static_cast<std::ptrdiff_t>(i), tmp); // format.cpp:1090-1091
    }

    if (!add_format && i >= 1) {
        // format.cpp:1093-1108: propagate the cleared bit backward through
        // runs that predate the new "off" marker but don't yet reflect it,
        // stopping as soon as one already does (or, per the original's
        // `j > 0`, before ever touching index 0).
        for (std::size_t j = i - 1; j > 0; --j) {
            if (get(runs[j])) break;
            set(runs[j], true);
        }
    }
}

} // namespace

auto strip_control_codes(std::string_view input) -> StripResult {
    StripResult result;
    result.plain.reserve(input.size());

    std::uint16_t fmt = 0;
    bool new_format_pending = false;

    std::size_t i = 0;
    while (i < input.size()) {
        if (is_format_control_char(input[i])) {
            new_format_pending = true;
            i = skip_one_format(input, i, fmt);
            continue;
        }

        // format.cpp:333-352: a run is recorded only when a literal
        // character actually follows a format change. A dangling control
        // sequence at the very end of `input` (new_format_pending still
        // true when the loop exits) intentionally produces no run — ported
        // faithfully from SzControlLess, not "fixed".
        if (new_format_pending) {
            result.runs.push_back(to_text_run(fmt, result.plain.size()));
            new_format_pending = false;
        }
        result.plain.push_back(input[i]);
        ++i;
    }

    return result;
}

auto mark_urls(const std::string& plain, std::vector<TextRun> runs) -> std::vector<TextRun> {
    const std::vector<UrlSpan> spans = find_urls(plain);
    if (spans.empty()) return runs;

    constexpr auto get_link = [](const TextRun& r) { return r.link; };
    constexpr auto set_link = [](TextRun& r, bool v) { r.link = v; };

    // IdentifyURLs (format.cpp:1216-1247): mark each URL span as linked,
    // then immediately turn the link back off at the span's end. find_urls
    // returns spans in left-to-right, non-overlapping order, so processing
    // them in sequence keeps `runs` sorted throughout.
    for (const UrlSpan& span : spans) {
        insert_bool_format(runs, /*add_format=*/true, span.offset, get_link, set_link);
        insert_bool_format(runs, /*add_format=*/false, span.offset + span.length, get_link, set_link);
    }

    return runs;
}

auto reslice_runs_for_line(const std::vector<TextRun>& runs, std::size_t line_start, std::size_t line_len)
    -> std::vector<TextRun> {
    // Step 1: PullFormattingOffsets(runs, line_start) (format.cpp:1140-1182).
    std::vector<TextRun> pulled;
    if (line_start == 0) {
        pulled = runs; // format.cpp:1147-1148 (CopyFormatting on zero delta).
    } else {
        TextRun latest{};
        bool have_latest = false;
        for (const TextRun& run : runs) {
            if (run.offset >= line_start) {
                if (pulled.empty() && have_latest) {
                    // format.cpp:1159-1166: carry over whatever format was
                    // active right before the cut, as an offset-0 run.
                    TextRun carry = latest;
                    carry.offset = 0;
                    pulled.push_back(carry);
                }
                TextRun shifted = run;
                shifted.offset = run.offset - line_start;
                pulled.push_back(shifted);
            } else {
                latest = run;
                have_latest = has_any_format(run); // format.cpp:1157, 1170
            }
        }
        if (pulled.empty() && have_latest) {
            // format.cpp:1173-1179: every run fell before line_start, but
            // one carried real formatting into the line.
            TextRun carry = latest;
            carry.offset = 0;
            pulled.push_back(carry);
        }
    }

    // Step 2: CutFormattingArray(pulled, line_len) (format.cpp:1114-1137):
    // drop anything at or past the line's length.
    std::vector<TextRun> result;
    result.reserve(pulled.size());
    for (const TextRun& run : pulled) {
        if (run.offset < line_len) result.push_back(run);
    }
    return result;
}

} // namespace comicchat
