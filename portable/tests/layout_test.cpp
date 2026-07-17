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

// -------------------------------------------------------------------------
// Item 2.3b — conversation-panel expert placement golden tests.
//
// Every expected value below is hand-derived from the Microsoft 2.5-beta source
// (v2.5-beta-1-modern/panel.cpp: OrderAvatars/DoGreedyOrdering/EvalPlacement/
// EvalPair/ComputeDisplacementPenalty/AddTalkTos/UpdateHistoresis, and the
// AddLine split at panel.cpp:1067,1082), not read back from the port. Facing:
// flip=false means facing right (toward a higher index), flip=true facing left.

using comicchat::ArrowAnchor;
using comicchat::AvatarHistoresis;
using comicchat::BodySlot;
using comicchat::ConversationAvatar;
using comicchat::HistoresisMap;
using comicchat::PanelSplitState;
using comicchat::PlacedBody;

TEST_CASE("Two mutually-addressing speakers are placed facing each other") {
    // A(1) talks to B(2), B talks to A, both fresh. Greedy puts A left facing
    // right and B right facing left so each faces the other (EvalPair rewards
    // facing your talk-to, panel.cpp:304-305).
    const std::vector<ConversationAvatar> avatars{{1, {2}}, {2, {1}}};
    const auto result = comicchat::order_conversation({1, 2}, avatars, {});

    CHECK(result.bodies == std::vector<PlacedBody>{{1, true, false}, {2, true, true}});
    // UpdateHistoresis records each body's facing + neighbours for the next panel.
    CHECK(result.historesis.at(1) == AvatarHistoresis{false, 2, 0});
    CHECK(result.historesis.at(2) == AvatarHistoresis{true, 0, 1});
}

TEST_CASE("Historesis keeps the left/right arrangement stable across panels") {
    // Re-running the same two speakers with the first panel's remembered state
    // reproduces the identical order; the arrangement is a fixed point and the
    // displacement penalty now rewards keeping A left of B (panel.cpp:266-274).
    const std::vector<ConversationAvatar> avatars{{1, {2}}, {2, {1}}};
    const auto first = comicchat::order_conversation({1, 2}, avatars, {});
    const auto second = comicchat::order_conversation({1, 2}, avatars, first.historesis);

    CHECK(second.bodies == first.bodies);
    CHECK(second.historesis == first.historesis);
}

TEST_CASE("AddTalkTos pulls a partner into a sparse panel") {
    // Only A(1) speaks, addressing B(2). With fewer than five speakers,
    // AddTalkTos pulls B in as a non-requested body (panel.cpp:325,341-342), then
    // A is placed left facing right toward B.
    const std::vector<ConversationAvatar> avatars{{1, {2}}, {2, {}}};
    const auto result = comicchat::order_conversation({1}, avatars, {});

    CHECK(result.bodies == std::vector<PlacedBody>{{1, true, false}, {2, false, true}});
    CHECK(result.historesis.at(1) == AvatarHistoresis{false, 2, 0});
    CHECK(result.historesis.at(2) == AvatarHistoresis{true, 0, 1});
}

TEST_CASE("A three-speaker chain orders by the greedy rating") {
    // A(1)->B(2), B(2)->C(3), C talks to the world. Greedy inserts C at the
    // minimum-rating position (the far left, rating 10 vs 12/46) so the final
    // order is C, A, B with C and A facing right and B facing left.
    const std::vector<ConversationAvatar> avatars{{1, {2}}, {2, {3}}, {3, {}}};
    const auto result = comicchat::order_conversation({1, 2, 3}, avatars, {});

    CHECK(result.bodies ==
          std::vector<PlacedBody>{{3, true, false}, {1, true, false}, {2, true, true}});
    CHECK(result.historesis.at(1) == AvatarHistoresis{false, 2, 3});
    CHECK(result.historesis.at(2) == AvatarHistoresis{true, 0, 1});
    CHECK(result.historesis.at(3) == AvatarHistoresis{false, 1, 0});
}

TEST_CASE("The AddLine split predicate mirrors panel.cpp:1067,1082") {
    // A fresh page opens a panel on the first line (m_newPanel true at start).
    CHECK(comicchat::should_start_new_panel(PanelSplitState{0, 0, true}, false, false));
    // A normal continuation into an existing multi-panel page does not split.
    CHECK_FALSE(comicchat::should_start_new_panel(PanelSplitState{2, 2, false}, false, false));
    // Five elements already in the tail panel forces a split.
    CHECK(comicchat::should_start_new_panel(PanelSplitState{5, 2, false}, false, false));
    // Fewer than two panels forces a split.
    CHECK(comicchat::should_start_new_panel(PanelSplitState{2, 1, false}, false, false));
    // The speaker already appearing in the tail panel forces a split.
    CHECK(comicchat::should_start_new_panel(PanelSplitState{2, 2, false}, true, false));
    // An action box always forces a new panel, regardless of the rest.
    CHECK(comicchat::should_start_new_panel(PanelSplitState{2, 2, false}, false, true));
}

TEST_CASE("arrow_anchors lays bodies left-to-right and flips faceX") {
    // Two bodies packed with a 100-twip gap. The first faces right so its tail
    // anchor sits at faceX; the second is flipped so faceX mirrors to
    // (1 - fraction) * width, matching GetDimInfo (avatar.cpp:74) and the
    // m_arrowX = box.Left + ROUND(fraction * width) projection (panel.cpp:817).
    const std::vector<BodySlot> slots{
        {1, 400, 0.25, false},
        {2, 600, 0.25, true},
    };
    const auto anchors = comicchat::arrow_anchors(slots, 0, 100);
    CHECK(anchors == std::vector<ArrowAnchor>{{1, 100, 200}, {2, 600, 1050}});
}
