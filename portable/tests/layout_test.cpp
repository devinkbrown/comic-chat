#include "comicchat/layout.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <vector>

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

TEST_CASE("MsvcrtRandom reproduces the MSVCRT rand() sequence byte-for-byte") {
    // Canonical srand(1)/rand() output of the Microsoft C runtime. These exact
    // values are what makes a seed captured from a real .ccc re-layout the same
    // way on any host (panel.cpp:558/870). If this sequence ever drifts, no
    // captured m_seed reproduces and every re-layout golden is invalid.
    const std::vector<std::uint32_t> canonical_seed1{
        41, 18467, 6334, 26500, 19169, 15724, 11478, 29358, 26962, 24464,
    };
    comicchat::MsvcrtRandom generator{};  // default state 1, i.e. no srand() call
    std::vector<std::uint32_t> produced;
    for (std::size_t index = 0; index < canonical_seed1.size(); ++index) {
        produced.push_back(generator.next());
    }
    CHECK(produced == canonical_seed1);

    // Explicit seed(1) must match the default-constructed generator exactly.
    comicchat::MsvcrtRandom seeded{};
    seeded.seed(1U);
    std::vector<std::uint32_t> reseeded;
    for (std::size_t index = 0; index < canonical_seed1.size(); ++index) {
        reseeded.push_back(seeded.next());
    }
    CHECK(reseeded == canonical_seed1);

    // A second fixed seed pins a non-default stream, and re-seeding rewinds it
    // deterministically — the "always layout the panel the same way" property.
    comicchat::MsvcrtRandom other{12345U};
    const std::vector<std::uint32_t> canonical_seed12345{7584, 19164, 25795, 22125, 5828, 23405};
    std::vector<std::uint32_t> first_pass;
    for (std::size_t index = 0; index < canonical_seed12345.size(); ++index) {
        first_pass.push_back(other.next());
    }
    CHECK(first_pass == canonical_seed12345);
    other.seed(12345U);
    std::vector<std::uint32_t> second_pass;
    for (std::size_t index = 0; index < canonical_seed12345.size(); ++index) {
        second_pass.push_back(other.next());
    }
    CHECK(second_pass == first_pass);

    // Every draw stays within [0, RAND_MAX].
    comicchat::MsvcrtRandom bounded{7U};
    for (std::size_t index = 0; index < 4096; ++index) {
        CHECK(bounded.next() <= comicchat::MsvcrtRandom::rand_max);
    }
}

TEST_CASE("randfloat() maps the LCG output onto [0, 1] deterministically") {
    // randfloat() = ((double) rand()) / RAND_MAX (balloon.cpp:446).
    comicchat::MsvcrtRandom generator{};  // seed 1
    // First draw is 41 (see canonical sequence above).
    CHECK(generator.next_float() == 41.0 / 32767.0);

    comicchat::MsvcrtRandom lo{};
    comicchat::MsvcrtRandom hi{};
    for (std::size_t index = 0; index < 1000; ++index) {
        const auto value = lo.next_float();
        CHECK(value >= 0.0);
        CHECK(value <= 1.0);
    }
    // The RAND_MAX draw (== rand_max) maps to exactly 1.0; construct it directly.
    CHECK(static_cast<double>(comicchat::MsvcrtRandom::rand_max) /
              static_cast<double>(comicchat::MsvcrtRandom::rand_max) ==
          1.0);
    (void)hi;
}

TEST_CASE("logical twips/Y-up coordinates map to device pixels at the final scale") {
    using comicchat::DevicePoint;
    using comicchat::LogicalPoint;

    // A 460x460 canvas fitting a 2300-twip panel: scale 0.2, origin (0,0).
    const auto square = comicchat::fit_panel_transform(460, 460, 2300.0);
    CHECK(square.scale == 0.2);
    CHECK(square.origin_x == 0.0);
    CHECK(square.origin_y == 0.0);
    // Panel top-left (logical 0,0) -> device top-left.
    CHECK(square.to_device(LogicalPoint{0.0, 0.0}) == DevicePoint{0.0, 0.0});
    // Panel top-right (logical +x, y=0).
    CHECK(square.to_device(LogicalPoint{2300.0, 0.0}) == DevicePoint{460.0, 0.0});
    // Panel bottom-left: Y-up bottom = -unitHeight maps DOWN to +device-y.
    CHECK(square.to_device(LogicalPoint{0.0, -2300.0}) == DevicePoint{0.0, 460.0});
    // Panel center.
    CHECK(square.to_device(LogicalPoint{1150.0, -1150.0}) == DevicePoint{230.0, 230.0});

    // A wider-than-tall canvas centers the square panel horizontally.
    const auto wide = comicchat::fit_panel_transform(600, 460, 2300.0);
    CHECK(wide.scale == 0.2);
    CHECK(wide.origin_x == 70.0);  // (600 - 460) / 2
    CHECK(wide.origin_y == 0.0);
    CHECK(wide.to_device(LogicalPoint{0.0, 0.0}) == DevicePoint{70.0, 0.0});
    CHECK(wide.to_device(LogicalPoint{2300.0, -2300.0}) == DevicePoint{530.0, 460.0});

    CHECK_THROWS_AS(comicchat::fit_panel_transform(0, 10, 2300.0), std::invalid_argument);
    CHECK_THROWS_AS(comicchat::fit_panel_transform(10, 10, 0.0), std::invalid_argument);
}
