#include "comicchat/layout.hpp"
#include "comicchat/render.hpp"
#include "comicchat/text.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("source-faithful title panel renders into a Cairo image canvas") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = comicchat::TextEngine::create(*font);
    REQUIRE(text.has_value());
    comicchat::Canvas canvas{460, 460};
    canvas.clear({1.0, 1.0, 1.0, 1.0});
    canvas.render_title_panel({"A Chat to Remember", "STARRING", {{"Alice"}, {"Bob", 0xb85c5cU}}}, **text);
    REQUIRE(canvas.pixels().size() == 460U * 460U);
    const auto white = std::uint32_t{0xffffffffU};
    CHECK(std::ranges::count_if(canvas.pixels(), [white](const auto pixel) { return pixel != white; }) > 1'000);
}

TEST_CASE("logical twips/Y-up rects fill the correct device pixels through the panel transform") {
    // ARGB32 opaque red as Cairo stores it after OVER-compositing (1,0,0,1)
    // onto opaque white.
    constexpr auto red = std::uint32_t{0xffff0000U};
    constexpr auto white = std::uint32_t{0xffffffffU};
    constexpr auto width = 460;

    const auto pixel_at = [](const comicchat::Canvas& canvas, const int x, const int y) {
        return canvas.pixels()[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)];
    };

    // The full 2300-twip panel covers the whole 460x460 canvas at scale 0.2.
    comicchat::Canvas full{width, width};
    full.clear({1.0, 1.0, 1.0, 1.0});
    full.fill_logical_rect(comicchat::Rect{0, -2300, 2300, 0}, {1.0, 0.0, 0.0, 1.0});
    CHECK(pixel_at(full, 230, 230) == red);
    CHECK(std::ranges::count(full.pixels(), red) == static_cast<long>(width) * width);

    // The upper-left LOGICAL quadrant {x:0..1150, y:0..-1150} must land in the
    // upper-left DEVICE quadrant (0..230, 0..230): this is the Y-up -> Y-down
    // flip. The opposite device corner stays white.
    comicchat::Canvas quadrant{width, width};
    quadrant.clear({1.0, 1.0, 1.0, 1.0});
    quadrant.fill_logical_rect(comicchat::Rect{0, -1150, 1150, 0}, {1.0, 0.0, 0.0, 1.0});
    CHECK(pixel_at(quadrant, 100, 100) == red);    // top-left device: painted
    CHECK(pixel_at(quadrant, 350, 350) == white);  // bottom-right device: untouched
    CHECK(pixel_at(quadrant, 100, 350) == white);  // bottom-left device: untouched
    CHECK(pixel_at(quadrant, 350, 100) == white);  // top-right device: untouched

    // The drawing pass is deterministic: the same logical input yields a
    // byte-identical frame.
    comicchat::Canvas repeat{width, width};
    repeat.clear({1.0, 1.0, 1.0, 1.0});
    repeat.fill_logical_rect(comicchat::Rect{0, -1150, 1150, 0}, {1.0, 0.0, 0.0, 1.0});
    CHECK(std::ranges::equal(repeat.pixels(), quadrant.pixels()));

    // Canvas::panel_transform exposes the same fit as the free function.
    const auto transform = full.panel_transform();
    CHECK(transform.scale == 0.2);
    CHECK(transform.origin_x == 0.0);
    CHECK(transform.origin_y == 0.0);
}

TEST_CASE("canvas rejects invalid and overflowing Cairo layouts before allocation") {
    CHECK_THROWS_AS(comicchat::Canvas(0, 1), std::invalid_argument);
    CHECK_THROWS_AS(comicchat::Canvas(1, 0), std::invalid_argument);
    CHECK_THROWS(comicchat::Canvas(std::numeric_limits<std::int32_t>::max(), 2));
}
