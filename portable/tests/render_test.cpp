#include "comicchat/render.hpp"
#include "comicchat/text.hpp"

#include <algorithm>
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

TEST_CASE("canvas rejects invalid and overflowing Cairo layouts before allocation") {
    CHECK_THROWS_AS(comicchat::Canvas(0, 1), std::invalid_argument);
    CHECK_THROWS_AS(comicchat::Canvas(1, 0), std::invalid_argument);
    CHECK_THROWS(comicchat::Canvas(std::numeric_limits<std::int32_t>::max(), 2));
}
