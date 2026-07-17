#include "comicchat/text.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace {
// A synthetic fixed-cell measure: every UTF-8 code point (spaces included) is
// `cell` units wide. Lets the pure wrap/ellipsis/bbox logic be pinned to exact
// hand-computed goldens without depending on any font's shaped advances.
auto monospace_measure(std::int32_t cell) -> comicchat::TextMeasure {
    return [cell](std::string_view text) -> std::int32_t {
        std::int32_t points = 0;
        for (std::size_t i = 0; i < text.size(); ++i)
            if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) ++points; // count non-continuation bytes
        return points * cell;
    };
}
} // namespace

TEST_CASE("ICU normalization is strict and deterministic") {
    const auto normalized = comicchat::normalize_utf8_nfc("Cafe\xCC\x81");
    REQUIRE(normalized.has_value());
    CHECK(*normalized == "Caf\xC3\xA9");

    const char invalid[]{static_cast<char>(0xc3), static_cast<char>(0x28)};
    CHECK_FALSE(comicchat::normalize_utf8_nfc(std::string_view{invalid, 2}).has_value());
}

TEST_CASE("FreeType and HarfBuzz shape Unicode text") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    CHECK(std::filesystem::path{*font}.filename() == "comic.ttf");
    auto engine = comicchat::TextEngine::create(*font);
    REQUIRE(engine.has_value());
    const auto glyphs = (*engine)->shape("Comic Chat \xE2\x98\x85", 28.0);
    REQUIRE(glyphs.has_value());
    CHECK(glyphs->size() >= 11);
    CHECK(glyphs->front().x_advance > 0.0);
}

// --- Item 2.4: CLabel font bounding-box logic (render-port-spec.md §2.4) -----
//
// These gate the Linux-provable half of 2.4: the exact bbox arithmetic
// (bottom = top - nLines*line_height - base_add, balloon.cpp:711), the line-wrap
// counting (::BreakIntoLines, balloon.cpp:363), and the CStarLabel single-line
// ellipsis (balloon.cpp:1197). Absolute font-PIXEL parity vs. the Win32 GDI font
// mapper is NOT asserted here — see the "deterministic, not GDI-identical" case.

TEST_CASE("compute_label_bbox reproduces the balloon.cpp:711 bottom formula (pure)") {
    const comicchat::FontMetrics fm{.line_height = 100, .base_add = 20};

    SECTION("centered width, hand-computed") {
        // desired = 400; center left = (400-250)/2 = 75; bottom = 0 - 3*100 - 20.
        const auto box = comicchat::compute_label_bbox(comicchat::Rect{.left = 0, .right = 400, .top = 0}, fm,
                                                       /*n_lines=*/3, /*max_line_width=*/250,
                                                       comicchat::LabelJustify::center);
        CHECK(box == comicchat::Rect{.left = 75, .bottom = -320, .right = 325, .top = 0});
    }

    SECTION("left-justified pins Left to the request") {
        const auto box = comicchat::compute_label_bbox(comicchat::Rect{.left = 0, .right = 400, .top = 0}, fm, 3,
                                                       250, comicchat::LabelJustify::left);
        CHECK(box == comicchat::Rect{.left = 0, .bottom = -320, .right = 250, .top = 0});
    }

    SECTION("negative base_add and a non-zero Top offset (Y-up)") {
        // base_add = -10, line_height = 90; bottom = 500 - 2*90 - (-10) = 330.
        const comicchat::FontMetrics off{.line_height = 90, .base_add = -10};
        const auto box = comicchat::compute_label_bbox(comicchat::Rect{.left = 100, .right = 600, .top = 500}, off,
                                                       2, 300, comicchat::LabelJustify::center);
        CHECK(box == comicchat::Rect{.left = 200, .bottom = 330, .right = 500, .top = 500});
    }
}

TEST_CASE("break_into_lines greedy word wrap matches ::BreakIntoLines (pure)") {
    const auto measure = monospace_measure(10); // 10 units / code point

    SECTION("wraps at word boundaries and records widths") {
        const auto lines = comicchat::break_into_lines(measure, /*max_width=*/100, "aaa bbb ccc ddd");
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == comicchat::TextLine{.text = "aaa bbb", .width = 70});
        CHECK(lines[1] == comicchat::TextLine{.text = "ccc ddd", .width = 70});
        CHECK(comicchat::widest_line_width(lines) == 70);
    }

    SECTION("force-breaks a single word wider than the box") {
        const auto lines = comicchat::break_into_lines(measure, /*max_width=*/50, "aaaaaaaa");
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == comicchat::TextLine{.text = "aaaaa", .width = 50});
        CHECK(lines[1] == comicchat::TextLine{.text = "aaa", .width = 30});
    }

    SECTION("explicit newlines are hard breaks, blank lines preserved") {
        const auto lines = comicchat::break_into_lines(measure, 100, "ab\n\ncd");
        REQUIRE(lines.size() == 3);
        CHECK(lines[0].text == "ab");
        CHECK(lines[1] == comicchat::TextLine{.text = "", .width = 0});
        CHECK(lines[2].text == "cd");
    }

    SECTION("caps at max_label_lines (balloon.h MAXLINES)") {
        const auto lines = comicchat::break_into_lines(measure, /*max_width=*/10, "a b c d e f g h i j k l m n o");
        CHECK(lines.size() == static_cast<std::size_t>(comicchat::max_label_lines));
    }
}

TEST_CASE("ellipsize_single_line reproduces CStarLabel DT_END_ELLIPSIS (pure)") {
    const auto measure = monospace_measure(10);

    CHECK(comicchat::ellipsize_single_line(measure, /*max_width=*/50, "abc") == "abc"); // fits: unchanged
    CHECK(comicchat::ellipsize_single_line(measure, /*max_width=*/50, "abcdef") == "ab..."); // "ab" + "..."
    CHECK(comicchat::ellipsize_single_line(measure, /*max_width=*/30, "abcdef") == "..."); // only ellipsis fits
    CHECK(comicchat::ellipsize_single_line(measure, /*max_width=*/20, "abcdef").empty()); // ellipsis too wide
}

TEST_CASE("build_font_metrics maps FreeType metrics through the CFontInfo formulas") {
    // NOTE: metric SOURCE is FreeType FT_Size_Metrics for the bundled Comic Neue
    // TTF, NOT GDI GetTextMetrics — so these are deterministic per TTF but NOT
    // byte-identical to Win32 (render-port-spec.md §2.4.c). We assert the
    // derivation RELATIONSHIPS (balloon.cpp:606-643), not absolute GDI pixels.
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto engine = comicchat::TextEngine::create(*font);
    REQUIRE(engine.has_value());

    const double size = 180.0;
    const auto raw = (*engine)->size_metrics(size);
    REQUIRE(raw.has_value());
    CHECK(raw->height == raw->ascent + raw->descent);
    CHECK(raw->external_leading >= 0);

    SECTION("title-style kern (n_leading != 0 -> top_offset 0)") {
        const std::int32_t n_leading = -220;
        const std::int32_t n_base_add = 120;
        const auto fm = comicchat::build_font_metrics(**engine, size, n_leading, n_base_add);
        REQUIRE(fm.has_value());
        CHECK(fm->leading == n_leading + raw->external_leading);   // balloon.cpp:624
        CHECK(fm->base_add == n_base_add - raw->external_leading); // balloon.cpp:625
        CHECK(fm->line_height == raw->height + fm->leading);       // balloon.cpp:640
        CHECK(fm->top_offset == 0);                                // n_leading != 0 (balloon.cpp:636)
        CHECK(fm->continuation_width > 0);                         // width of "..." (balloon.cpp:642)
    }

    SECTION("shout kern (0,0) -> FAREAST top_offset") {
        const auto fm = comicchat::build_font_metrics(**engine, size, /*n_leading=*/0, /*n_base_add=*/0);
        REQUIRE(fm.has_value());
        CHECK(fm->top_offset == comicchat::far_east_top_offset); // n_leading == 0 (balloon.cpp:638)
        CHECK(fm->leading == raw->external_leading);
        CHECK(fm->base_add == -raw->external_leading);
        CHECK(fm->line_height == raw->height + raw->external_leading);
    }

    SECTION("deterministic per TTF (not a GDI-parity claim)") {
        const auto a = comicchat::build_font_metrics(**engine, size, -40, 30);
        const auto b = comicchat::build_font_metrics(**engine, size, -40, 30);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        CHECK(*a == *b);
    }
}

TEST_CASE("real-font wrap -> bbox integration honors bottom = top - nLines*line_height - base_add") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto engine = comicchat::TextEngine::create(*font);
    REQUIRE(engine.has_value());

    const double size = 96.0;
    const auto fm = comicchat::build_font_metrics(**engine, size, -40, 30);
    REQUIRE(fm.has_value());

    const auto measure = comicchat::measure_text_width(**engine, size);
    const comicchat::Rect request{.left = 0, .right = 1200, .top = 0};
    const auto lines = comicchat::break_into_lines(measure, request.right - request.left,
                                                   "Comic Chat wraps this sentence across several lines");
    REQUIRE(lines.size() >= 2); // the string is wider than the box -> multiple lines

    const auto widest = comicchat::widest_line_width(lines);
    const auto box = comicchat::compute_label_bbox(request, *fm, static_cast<std::int32_t>(lines.size()), widest,
                                                   comicchat::LabelJustify::center);
    CHECK(box.top == 0);
    CHECK(box.bottom == box.top - static_cast<std::int32_t>(lines.size()) * fm->line_height - fm->base_add);
    CHECK(box.right - box.left == widest);
    for (const auto& line : lines) CHECK(line.width <= request.right - request.left);
}
