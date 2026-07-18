// Gate tests for per-run balloon text formatting (bold/italic/underline/color/
// link) threaded from the message model into the render_panel balloon draw.
//
// Covers:
//   * break_into_lines_formatted: runs slice per wrapped line, and an empty run
//     list degenerates to the plain break_into_lines result.
//   * The render zero-behavior-change guard: a line whose runs are empty renders
//     byte-identically to the same line drawn as a single default-format run
//     (the unchanged draw_shaped path) -- the most important regression proof.
//   * A bold span produces strictly more dark ink than the same text plain
//     (faux-bold ink-density heuristic on the rendered pixels).
//   * A URL/link span renders in the blue link color absent from the plain draw.

#include "comicchat/balloon.hpp"
#include "comicchat/formatting.hpp"
#include "comicchat/page.hpp"
#include "comicchat/render.hpp"
#include "comicchat/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

using comicchat::Balloon;
using comicchat::BalloonMode;
using comicchat::Canvas;
using comicchat::Panel;
using comicchat::TextEngine;
using comicchat::TextLine;
using comicchat::TextRun;

constexpr int kCanvas = 460;

// A single say-balloon holding one wide text line at a fixed size, positioned in
// the upper-left of the panel so its glyphs land well inside the canvas. No
// cloud outline/tail: we only exercise the text-draw loop.
auto text_balloon(std::string line_text, std::vector<TextRun> runs) -> Panel {
    Balloon balloon{};
    balloon.kind = {BalloonMode::say, false};
    balloon.text = line_text;
    TextLine tl{};
    tl.text = std::move(line_text);
    tl.width = 1600;
    tl.runs = std::move(runs);
    balloon.lines.push_back(std::move(tl));
    balloon.bbox = comicchat::Rect{200, -600, 2100, -100}; // top=-100 text top
    balloon.line_height = 300;
    balloon.text_size = 240.0; // panel twips; matches the live say font size
    Panel panel{};
    panel.balloons.push_back(std::move(balloon));
    return panel;
}

auto render(const Panel& panel, TextEngine& text) -> Canvas {
    Canvas canvas{kCanvas, kCanvas};
    canvas.clear({1.0, 1.0, 1.0, 1.0});
    canvas.render_panel(panel, text);
    return canvas;
}

// Count pixels that are meaningfully darker than the white background (text ink).
auto dark_ink(const Canvas& canvas) -> long {
    return std::ranges::count_if(canvas.pixels(), [](const std::uint32_t pixel) {
        const auto r = (pixel >> 16U) & 0xffU;
        const auto g = (pixel >> 8U) & 0xffU;
        const auto b = pixel & 0xffU;
        return r < 160U && g < 160U && b < 160U;
    });
}

// Count blue-dominant pixels (the link ink {0,0,0.9} composited on white).
auto blue_ink(const Canvas& canvas) -> long {
    return std::ranges::count_if(canvas.pixels(), [](const std::uint32_t pixel) {
        const auto r = (pixel >> 16U) & 0xffU;
        const auto g = (pixel >> 8U) & 0xffU;
        const auto b = pixel & 0xffU;
        return b > 120U && b > r + 60U && b > g + 60U;
    });
}

} // namespace

TEST_CASE("break_into_lines_formatted slices runs per line and matches plain wrapping") {
    // Monospace synthetic measure: 10 units per byte, so wrapping is TTF-free.
    const comicchat::TextMeasure measure = [](std::string_view s) {
        return static_cast<std::int32_t>(s.size()) * 10;
    };

    // "aaa BBB ccc" with a bold run over "BBB" (offset 4..6) wraps at width 70
    // into "aaa BBB" / "ccc".
    const std::string plain = "aaa BBB ccc";
    std::vector<TextRun> runs{TextRun{.offset = 4, .bold = true}, TextRun{.offset = 7, .bold = false}};

    const auto plain_lines = comicchat::break_into_lines(measure, 70, plain);
    const auto fmt_lines = comicchat::break_into_lines_formatted(measure, 70, plain, runs);

    REQUIRE(fmt_lines.size() == plain_lines.size());
    for (std::size_t i = 0; i < fmt_lines.size(); ++i) {
        CHECK(fmt_lines[i].text == plain_lines[i].text);
        CHECK(fmt_lines[i].width == plain_lines[i].width);
    }
    REQUIRE(fmt_lines.size() == 2);
    // Line 0 "aaa BBB": the bold run rebased to line-local offset 4 survives.
    REQUIRE(fmt_lines[0].runs.size() >= 1);
    const auto& l0 = fmt_lines[0].runs;
    CHECK(std::ranges::any_of(l0, [](const TextRun& r) { return r.bold && r.offset == 4; }));
    // Line 1 "ccc": no bold (the run ended before it).
    CHECK(std::ranges::none_of(fmt_lines[1].runs, [](const TextRun& r) { return r.bold; }));

    // Empty runs -> identical to the plain path (degenerate case).
    const auto empty_fmt = comicchat::break_into_lines_formatted(measure, 70, plain, {});
    REQUIRE(empty_fmt.size() == plain_lines.size());
    for (std::size_t i = 0; i < empty_fmt.size(); ++i) {
        CHECK(empty_fmt[i].text == plain_lines[i].text);
        CHECK(empty_fmt[i].runs.empty());
    }
}

TEST_CASE("empty-runs line renders byte-identically to a single default-format run") {
    // THE REGRESSION GUARD: the empty-runs branch calls the unchanged draw_shaped.
    // A single whole-line run with NO overrides (no fg, upright, regular weight)
    // must reproduce those exact pixels through the new per-run draw path -- i.e.
    // the formatted path is a faithful superset and the no-format case is pixel-
    // identical to before this feature.
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = comicchat::TextEngine::create(*font);
    REQUIRE(text.has_value());

    const std::string line = "HELLO COMIC WORLD";
    const auto plain = render(text_balloon(line, {}), **text);
    // One default run over the whole line: no fg, bold/italic/underline all false.
    const auto default_run =
        render(text_balloon(line, {TextRun{.offset = 0}}), **text);

    CHECK(std::ranges::equal(plain.pixels(), default_run.pixels()));
    // Sanity: this path actually drew text.
    CHECK(dark_ink(plain) > 200);
}

TEST_CASE("bold run produces strictly more dark ink than the same text plain") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = comicchat::TextEngine::create(*font);
    REQUIRE(text.has_value());

    // \x02 = ctl_bold. strip_control_codes -> plain "bold plain", runs marking the
    // "bold" span (offset 0..3) bold and "plain" regular.
    const auto stripped = comicchat::strip_control_codes("\x02" "bold\x02 plain");
    REQUIRE(stripped.plain == "bold plain");
    REQUIRE_FALSE(stripped.runs.empty());

    const auto formatted = render(text_balloon(stripped.plain, stripped.runs), **text);
    const auto plain = render(text_balloon(stripped.plain, {}), **text);

    // Faux-bold strokes the "bold" span, so the formatted frame carries strictly
    // more dark ink than the identical text drawn entirely regular-weight.
    CHECK(dark_ink(formatted) > dark_ink(plain));
    CHECK(dark_ink(plain) > 200); // both actually rendered glyphs
}

TEST_CASE("URL span renders in the blue link color") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = comicchat::TextEngine::create(*font);
    REQUIRE(text.has_value());

    const std::string plain = "see http://x.co now";
    const auto runs = comicchat::mark_urls(plain, {});
    REQUIRE(std::ranges::any_of(runs, [](const TextRun& r) { return r.link; }));

    const auto linked = render(text_balloon(plain, runs), **text);
    const auto no_link = render(text_balloon(plain, {}), **text);

    // The link span paints blue ink (and an underline) that the plain draw lacks.
    CHECK(blue_ink(linked) > 20);
    CHECK(blue_ink(no_link) == 0);
}
