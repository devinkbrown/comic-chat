#include "comicchat/layout.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

TEST_CASE("Microsoft two-column panel geometry is preserved") {
    using comicchat::Rect;
    CHECK(comicchat::panel_rect(0, 0, 0) == Rect{0, -2300, 2300, 0});
    CHECK(comicchat::panel_rect(1, 0, 0) == Rect{2444, -2300, 4744, 0});
    CHECK(comicchat::panel_rect(2, 0, 0) == Rect{0, -4744, 2300, -2444});
    CHECK(comicchat::page_bounds(3, 10, 20) == Rect{10, -4724, 4754, 20});
    CHECK_THROWS_AS(comicchat::page_bounds(0, 0, 0), std::invalid_argument);
}

TEST_CASE("AddStarsAux keeps self then active senders and skips missing icons") {
    const std::vector<comicchat::Participant> participants{
        {"Me", true, false, 1, true},
        {"Gone", false, true, 99, true},
        {"Amy", false, false, 2, true},
        {"Chatter", false, false, 10, true},
        {"NoIcon", false, false, 100, false},
    };
    CHECK(comicchat::order_stars(participants, 3) == std::vector<std::size_t>{0, 3, 2});
}
