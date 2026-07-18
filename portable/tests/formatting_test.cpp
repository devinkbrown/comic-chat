#include "comicchat/formatting.hpp"

#include "comicchat/urldetect.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using comicchat::ctl_bold;
using comicchat::ctl_color;
using comicchat::find_urls;
using comicchat::mark_urls;
using comicchat::palette_color;
using comicchat::reslice_runs_for_line;
using comicchat::Rgb8;
using comicchat::strip_control_codes;
using comicchat::TextRun;

TEST_CASE("strip_control_codes toggles bold and records a run per literal-following-change") {
    const std::string input = std::string() + "a" + ctl_bold + "bold" + ctl_bold + "b";
    const auto result = strip_control_codes(input);

    CHECK(result.plain == "aboldb");
    REQUIRE(result.runs.size() == 2);
    CHECK(result.runs[0] == TextRun{.offset = 1, .bold = true});
    CHECK(result.runs[1] == TextRun{.offset = 5, .bold = false});
}

TEST_CASE("strip_control_codes parses a single-digit foreground color and a lonely reset") {
    // "^c4Red^cX": foreground color 4 on "Red", reset back to no color for "X".
    const std::string input = std::string() + ctl_color + "4Red" + ctl_color + "X";
    const auto result = strip_control_codes(input);

    CHECK(result.plain == "RedX");
    REQUIRE(result.runs.size() == 2);
    CHECK(result.runs[0].offset == 0);
    REQUIRE(result.runs[0].fg.has_value());
    CHECK(*result.runs[0].fg == 4);
    CHECK_FALSE(result.runs[0].bg.has_value());

    CHECK(result.runs[1].offset == 3);
    CHECK_FALSE(result.runs[1].fg.has_value());
    CHECK_FALSE(result.runs[1].bg.has_value());
}

TEST_CASE("strip_control_codes parses two-digit foreground,background color pairs") {
    // "^c04,12X": foreground 4 (04 % 16), background 12 (12 % 16).
    const std::string input = std::string() + ctl_color + "04,12X";
    const auto result = strip_control_codes(input);

    CHECK(result.plain == "X");
    REQUIRE(result.runs.size() == 1);
    REQUIRE(result.runs[0].fg.has_value());
    REQUIRE(result.runs[0].bg.has_value());
    CHECK(*result.runs[0].fg == 4);
    CHECK(*result.runs[0].bg == 12);
}

TEST_CASE("strip_control_codes drops a dangling control sequence with nothing after it") {
    // A trailing ^b with no following literal character produces no run,
    // faithfully matching SzControlLess (format.cpp:333-352): the Add() call
    // only happens inside the `default:` (literal-character) branch.
    const std::string input = std::string() + "hi" + ctl_bold;
    const auto result = strip_control_codes(input);

    CHECK(result.plain == "hi");
    CHECK(result.runs.empty());
}

TEST_CASE("strip_control_codes on empty input returns empty plain text and no runs") {
    const auto result = strip_control_codes("");
    CHECK(result.plain.empty());
    CHECK(result.runs.empty());
}

TEST_CASE("mark_urls layers a link run over a detected URL span") {
    const std::string plain = "see http://example.com here";
    const auto spans = find_urls(plain);
    REQUIRE(spans.size() == 1);

    const auto runs = mark_urls(plain, {});
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].offset == spans[0].offset);
    CHECK(runs[0].link);
    CHECK(runs[1].offset == spans[0].offset + spans[0].length);
    CHECK_FALSE(runs[1].link);
}

TEST_CASE("mark_urls inherits surrounding formatting at the insertion point") {
    const std::string plain = "see http://example.com here";
    const auto spans = find_urls(plain);
    REQUIRE(spans.size() == 1);

    // The whole message starts bold; mark_urls must fold that inherited
    // format into both the link-on and link-off entries it inserts
    // (InsertFormat, format.cpp:1075-1081).
    std::vector<TextRun> runs = {TextRun{.offset = 0, .bold = true}};
    const auto marked = mark_urls(plain, runs);

    REQUIRE(marked.size() == 3);
    CHECK(marked[0] == TextRun{.offset = 0, .bold = true});
    CHECK(marked[1].offset == spans[0].offset);
    CHECK(marked[1].bold);
    CHECK(marked[1].link);
    CHECK(marked[2].offset == spans[0].offset + spans[0].length);
    CHECK(marked[2].bold);
    CHECK_FALSE(marked[2].link);
}

TEST_CASE("mark_urls with no URLs in the text returns runs unchanged") {
    std::vector<TextRun> runs = {TextRun{.offset = 0, .italic = true}};
    const auto marked = mark_urls("no links here", runs);
    CHECK(marked == runs);
}

TEST_CASE("reslice_runs_for_line rebases offsets and drops runs outside the window") {
    // Full text: "Hello World" - bold starts at offset 6 ("World").
    const std::vector<TextRun> runs = {TextRun{.offset = 6, .bold = true}};

    SECTION("the first line ('Hello ', [0,6)) sees no bold at all") {
        const auto local = reslice_runs_for_line(runs, /*line_start=*/0, /*line_len=*/6);
        CHECK(local.empty());
    }

    SECTION("the second line ('World', [6,11)) sees bold from its start") {
        const auto local = reslice_runs_for_line(runs, /*line_start=*/6, /*line_len=*/5);
        REQUIRE(local.size() == 1);
        CHECK(local[0] == TextRun{.offset = 0, .bold = true});
    }
}

TEST_CASE("reslice_runs_for_line carries an already-active format into a later line as offset 0") {
    // Formatting turned on mid-way through an earlier line; a later line
    // that starts after that point must still see it, at its own offset 0
    // (PullFormattingOffsets' wLatestFormat carry-over, format.cpp:1157-1179).
    const std::vector<TextRun> runs = {TextRun{.offset = 2, .bold = true}};

    const auto local = reslice_runs_for_line(runs, /*line_start=*/5, /*line_len=*/5);
    REQUIRE(local.size() == 1);
    CHECK(local[0] == TextRun{.offset = 0, .bold = true});
}

TEST_CASE("reslice_runs_for_line does not carry over a default (no-op) format") {
    // The run at offset 2 turns nothing on (has_any_format is false), so no
    // synthetic offset-0 carry-over run should be created.
    const std::vector<TextRun> runs = {TextRun{.offset = 2}};

    const auto local = reslice_runs_for_line(runs, /*line_start=*/5, /*line_len=*/5);
    CHECK(local.empty());
}

TEST_CASE("reslice_runs_for_line splits multiple runs across a line boundary") {
    // Full text: "AAAABBBBCC" - bold [0,4), italic [4,8), underline [8,10).
    const std::vector<TextRun> runs = {
        TextRun{.offset = 0, .bold = true},
        TextRun{.offset = 4, .italic = true},
        TextRun{.offset = 8, .underline = true},
    };

    const auto line0 = reslice_runs_for_line(runs, 0, 4);
    REQUIRE(line0.size() == 1);
    CHECK(line0[0] == TextRun{.offset = 0, .bold = true});

    // The run that starts exactly at line_start (offset 4, shifted to 0)
    // still gets a carried-over offset-0 entry prepended ahead of it: the
    // original PullFormattingOffsets only checks "is this the first
    // qualifying entry we're adding" (format.cpp:1159), not whether that
    // entry's own shifted offset is already 0. The redundant pair at offset
    // 0 is harmless for a forward-scanning renderer (the later entry always
    // wins), but it is faithfully reproduced here rather than "optimized
    // away".
    const auto line1 = reslice_runs_for_line(runs, 4, 4);
    REQUIRE(line1.size() == 2);
    CHECK(line1[0] == TextRun{.offset = 0, .bold = true});
    CHECK(line1[1] == TextRun{.offset = 0, .italic = true});

    const auto line2 = reslice_runs_for_line(runs, 8, 2);
    REQUIRE(line2.size() == 2);
    CHECK(line2[0] == TextRun{.offset = 0, .italic = true});
    CHECK(line2[1] == TextRun{.offset = 0, .underline = true});
}

TEST_CASE("palette_color reproduces GetRBGColor's 16-entry mIRC-compatible table") {
    CHECK(palette_color(0) == Rgb8{255, 255, 255});  // white
    CHECK(palette_color(4) == Rgb8{255, 0, 0});       // red
    CHECK(palette_color(12) == Rgb8{0, 0, 255});      // blue
    // Index 1 has no explicit `case` upstream (format.cpp:902-939) and falls
    // through to `default: RGB(0,0,0)`, same as any out-of-range code.
    CHECK(palette_color(1) == Rgb8{0, 0, 0});
    CHECK(palette_color(255) == Rgb8{0, 0, 0});
}
