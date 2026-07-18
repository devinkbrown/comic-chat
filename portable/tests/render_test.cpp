#include "comicchat/avatar_assets.hpp"
#include "comicchat/backdrop.hpp"
#include "comicchat/balloon.hpp"
#include "comicchat/comic_page.hpp"
#include "comicchat/layout.hpp"
#include "comicchat/page.hpp"
#include "comicchat/render.hpp"
#include "comicchat/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <catch2/catch_approx.hpp>
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

// ===================================================================
// Item 2.1 — balloon geometry (render-port-spec.md §2.1). The gates below are
// pure deterministic integer math, proven bit-exact on Linux: the cloud-spline
// control points / wavy permutation, the beta-spline bezier expansion, the tail
// anchor arithmetic, the RNG-driven cloud estimate, and shape selection. (Exact
// GDI-PolyBezier-vs-Cairo cloud PIXELS remain an MSVC/visual follow-up.)
// ===================================================================

namespace {

using comicchat::BalloonPoint;
using comicchat::FontMetrics;
using comicchat::TextLine;

// The ragged three-line label used across the deterministic goldens. Widths are
// fixed twips so the geometry is independent of the bundled TTF.
auto golden_lines() -> std::vector<TextLine> {
    return {TextLine{"HELLO THERE", 1200}, TextLine{"THIS IS A", 900}, TextLine{"COMIC", 600}};
}

auto golden_font() -> FontMetrics {
    FontMetrics font{};
    font.line_height = 400;
    font.base_add = 60;
    font.top_offset = 0;
    return font;
}

} // namespace

TEST_CASE("ShiftLines/GetFilters build the exact ragged-text staircase (balloon.cpp:768,482)") {
    const auto lines = golden_lines();
    const auto offsets = comicchat::shift_line_offsets(lines, comicchat::LabelJustify::center);
    CHECK(offsets == std::vector<int>{0, 150, 300});  // (max_width - width_i)/2

    const auto filters = comicchat::get_filters(lines, offsets);
    REQUIRE(filters.left.size() == 3);
    REQUIRE(filters.right.size() == 3);
    CHECK(filters.left[0].x == 0);
    CHECK(filters.left[1].x == 150);
    CHECK(filters.left[2].x == 300);
    CHECK(filters.right[0].x == 1200);
    CHECK(filters.right[1].x == 1050);
    CHECK(filters.right[2].x == 900);
    for (int i = 0; i < 3; ++i) {
        CHECK(filters.left[static_cast<std::size_t>(i)].start == i);
        CHECK(filters.left[static_cast<std::size_t>(i)].end == i);
    }
}

TEST_CASE("PermuteFilters assigns Y from the font metrics and returns finalY (balloon.cpp:531)") {
    const auto lines = golden_lines();
    const auto offsets = comicchat::shift_line_offsets(lines, comicchat::LabelJustify::center);
    auto filters = comicchat::get_filters(lines, offsets);
    const int final_y = comicchat::permute_filters(filters, golden_font());

    // First corner: TOPBORDER + YBORDER + top_offset = -20 + 40 + 0 = 20.
    // Interior corners drop by YBORDER + base_add below each baseY step.
    CHECK(filters.left[0].x == -100);   // 0 - XBORDER
    CHECK(filters.left[0].y == 20);
    CHECK(filters.left[1].y == -500);
    CHECK(filters.left[2].y == -900);
    CHECK(filters.right[0].x == 1300);  // 1200 + XBORDER
    CHECK(filters.right[0].y == 20);
    CHECK(filters.right[2].y == -900);
    CHECK(final_y == -1280);
}

TEST_CASE("CreateBalloonSpline emits the byte-exact cloud control-point stream (balloon.cpp:1839)") {
    const auto lines = golden_lines();
    const auto offsets = comicchat::shift_line_offsets(lines, comicchat::LabelJustify::center);
    auto filters = comicchat::get_filters(lines, offsets);
    const int final_y = comicchat::permute_filters(filters, golden_font());
    const auto cps = comicchat::create_balloon_spline(filters, final_y);

    const std::vector<BalloonPoint> expected{
        {-100, 20},   {-100, -500}, {50, -500},   {50, -900},    {200, -900},  {200, -1280},
        {600, -1350}, {1000, -1280}, {1000, -900}, {1150, -900},  {1150, -500}, {1300, -500},
        {1300, 20},   {950, 90},    {600, 20},    {250, 90},
    };
    CHECK(cps == expected);

    // The wavy bumps at [6], [13], [15] are the AddWavies scallops
    // (VWAVEHEIGHT/HWAVEHEIGHT = 70) inserted along the long bottom/top edges.
    CHECK(cps[6] == BalloonPoint{600, -1350});
    CHECK(cps[13] == BalloonPoint{950, 90});
    CHECK(cps[15] == BalloonPoint{250, 90});

    const auto box = comicchat::cloud_bbox(cps);
    CHECK(box == comicchat::Rect{-100, -1350, 1300, 90});
}

TEST_CASE("Closed beta-spline expansion matches the CBeta bezier port (spline.cpp:68,169)") {
    const auto lines = golden_lines();
    const auto offsets = comicchat::shift_line_offsets(lines, comicchat::LabelJustify::center);
    auto filters = comicchat::get_filters(lines, offsets);
    const int final_y = comicchat::permute_filters(filters, golden_font());
    const auto cps = comicchat::create_balloon_spline(filters, final_y);
    const auto bez = comicchat::beta_closed_bezier(cps);

    // BezierCount() = 3*nCps + 1 for a closed CBeta (spline.h:17,55).
    REQUIRE(bez.size() == static_cast<std::size_t>(3 * cps.size() + 1));
    CHECK(bez[0] == BalloonPoint{-59, -33});
    CHECK(bez[1] == BalloonPoint{-100, -102});
    CHECK(bez[2] == BalloonPoint{-100, -377});
    CHECK(bez[3] == BalloonPoint{-83, -439});
    // A closed spline's expansion returns to its start.
    CHECK(bez.back() == BalloonPoint{-59, -32});
}

TEST_CASE("Open beta-spline expansion uses the OPEN knot model (spline.cpp:55,241)") {
    const auto lines = golden_lines();
    const auto offsets = comicchat::shift_line_offsets(lines, comicchat::LabelJustify::center);
    auto filters = comicchat::get_filters(lines, offsets);
    const int final_y = comicchat::permute_filters(filters, golden_font());
    const auto cps = comicchat::create_balloon_spline(filters, final_y);

    const auto open = comicchat::beta_open_bezier(cps);
    // BezierCount() = 3*nCps + 4 for an OPEN CBeta (spline.h:17,55) -- the classic
    // off-by-one guard: closed is 3*nCps + 1, open is three points longer.
    REQUIRE(open.size() == static_cast<std::size_t>(3 * cps.size() + 4));
    CHECK(open.size() == comicchat::beta_closed_bezier(cps).size() + 3);
    // An open spline does NOT loop back to its start (unlike the closed one).
    CHECK(open.front() != open.back());
}

TEST_CASE("BreakSpline opens the cloud bottom at the tail throat (balloon.cpp:451-479)") {
    const auto lines = golden_lines();
    const auto offsets = comicchat::shift_line_offsets(lines, comicchat::LabelJustify::center);
    auto filters = comicchat::get_filters(lines, offsets);
    const int final_y = comicchat::permute_filters(filters, golden_font());
    const auto cps = comicchat::create_balloon_spline(filters, final_y);
    const auto closed = comicchat::beta_closed_bezier(cps);

    // Break near the horizontal centre of the cloud on the bottom row.
    const auto broken = comicchat::break_spline_open(cps, closed, 600, final_y);

    // The rewritten control array yields a valid OPEN beta bezier (size 3n+4).
    REQUIRE(broken.outline_open.size() >= 4);
    CHECK((broken.outline_open.size() - 4) % 3 == 0);
    // The two real wavy-bottom gap endpoints exist and straddle the break column,
    // so the outline runs from the right edge over the top to the left edge and
    // never strokes across the gap between them.
    CHECK(broken.gap_left != broken.gap_right);
    CHECK(broken.gap_left.x < broken.gap_right.x);
    // Both gap endpoints sit along the low (bottom) part of the cloud.
    const auto box = comicchat::cloud_bbox(cps);
    const int mid_y = (box.top + box.bottom) / 2;
    CHECK(broken.gap_left.y < mid_y);
    CHECK(broken.gap_right.y < mid_y);
    // The open outline is not a closed loop.
    CHECK(broken.outline_open.front() != broken.outline_open.back());
}

TEST_CASE("AddArrow tail anchors at the speaker and clamps to 45 degrees (balloon.cpp:1538)") {
    comicchat::TailInput in{};
    in.arrow_x = 1500;
    in.speaker_top = -1200;
    in.bbox_left = 300;
    in.bbox_top = -20;
    in.route_left = 400;
    in.route_right = 1800;
    in.cloud_bottom = -900;
    in.last_line_left = 250;
    in.last_line_width = 600;

    const auto tail = comicchat::compute_tail(in);
    // bottom2 = (arrowX, speaker_top + 200).
    CHECK(tail.anchor == BalloonPoint{1500, -1000});
    // The route-midpoint break is well inside the last line, so it is kept; the
    // (-400, +100) tail vector exceeds 45 deg, so the angle clamps and xbreak is
    // recomputed as cos(3pi/4)*100 + 1500 - 300 = 1129.
    CHECK(tail.xbreak == 1129);
    CHECK(tail.tip == BalloonPoint{1429, -900});
    CHECK(tail.angle == Catch::Approx(2.356194).epsilon(1e-5));

    // A short tail is stretched to at least MINTAILHEIGHT (= 100).
    comicchat::TailInput shorty = in;
    shorty.cloud_bottom = -960;  // only 40 above the anchor
    const auto stretched = comicchat::compute_tail(shorty);
    CHECK(stretched.anchor.y == stretched.tip.y - comicchat::balloon_min_tail_height);
}

TEST_CASE("GetCloudEstimate draws goal width and overlap x in the source rand() order (panel.cpp:888)") {
    comicchat::CloudEstimateInput in{};
    in.text_extent = 2000;
    in.text_height = 400;
    in.line_height = 400;
    in.widest_word = 700;
    in.free_left = 60;
    in.free_right = 2240;
    in.free_top = 0;
    in.free_bottom = -1150;
    in.lowest_prev_bottom = 0;
    in.arrow_x = 1500;

    comicchat::MsvcrtRandom rng{12345U};
    const auto est = comicchat::cloud_estimate(rng, in);
    CHECK(est.goal_width == 1983);
    CHECK(est.left == 257);
    CHECK(est.right == 2240);

    // Determinism: the same seed reproduces the same estimate byte-for-byte.
    comicchat::MsvcrtRandom repeat{12345U};
    CHECK(comicchat::cloud_estimate(repeat, in) == est);

    // A one-liner (<= ONELINETHRESHOLD) keeps its natural length and consumes
    // only the single x-placement draw.
    comicchat::CloudEstimateInput one = in;
    one.text_extent = 300;
    comicchat::MsvcrtRandom rng2{999U};
    const auto liner = comicchat::cloud_estimate(rng2, one);
    CHECK(liner.goal_width == 500);  // min(300 + 200, ...)
    CHECK(liner.left == 1050);
    CHECK(liner.right == 1550);
}

TEST_CASE("DockAtTop pins a near-top balloon at height + TOPBORDER (balloon.cpp:1306)") {
    const auto docked = comicchat::dock_at_top(comicchat::Rect{100, -500, 900, 0}, 0);
    CHECK(docked.top == comicchat::balloon_topborder);      // 0 + (-20)
    CHECK(docked.top - docked.bottom == 500);               // height preserved
    CHECK(docked == comicchat::Rect{100, -520, 900, -20});
}

TEST_CASE("Whisper/think/box shape selection follows MakeBalloon (panel.cpp:1039)") {
    using comicchat::BalloonMode;
    CHECK(comicchat::select_balloon_mode(comicchat::bm_say) ==
          comicchat::BalloonShapeKind{BalloonMode::say, false});
    CHECK(comicchat::select_balloon_mode(comicchat::bm_whisper) ==
          comicchat::BalloonShapeKind{BalloonMode::whisper, true});
    CHECK(comicchat::select_balloon_mode(comicchat::bm_think) ==
          comicchat::BalloonShapeKind{BalloonMode::think, false});
    CHECK(comicchat::select_balloon_mode(comicchat::bm_action) ==
          comicchat::BalloonShapeKind{BalloonMode::action, false});
    // BM_ACTION|BM_WHISPER -> dashed box (panel.cpp:1053).
    CHECK(comicchat::select_balloon_mode(comicchat::bm_action | comicchat::bm_whisper) ==
          comicchat::BalloonShapeKind{BalloonMode::action, true});
}

TEST_CASE("Think bubbles shrink along the vector to the speaker (balloon.cpp:1966)") {
    const auto bubbles = comicchat::think_bubbles(BalloonPoint{1000, -800}, BalloonPoint{1500, -2000});
    // nBubbles = (deltaY + INTERBUBBLE) / (BUBBLEHEIGHT + INTERBUBBLE) = 1300/250.
    REQUIRE(bubbles.size() == 5);
    CHECK(bubbles.front().center == BalloonPoint{1471, -1931});
    CHECK(bubbles.front().radius == comicchat::balloon_bubble_height / 2);
    CHECK(bubbles.front().width_pad == 0);
    // The pad grows monotonically toward the end bubble.
    CHECK(bubbles.back().width_pad > bubbles.front().width_pad);
}

TEST_CASE("Action box outline and bbox inset by XBOXDELTA/YBOXDELTA (balloon.cpp:2018,2042)") {
    const comicchat::Rect text_bbox{0, -800, 1200, 0};
    const auto box = comicchat::box_cloud_bbox(text_bbox);
    CHECK(box == comicchat::Rect{-comicchat::balloon_xbox_delta, -800 - comicchat::balloon_ybox_delta,
                                 1200 + comicchat::balloon_xbox_delta, comicchat::balloon_ybox_delta});
    const auto outline = comicchat::box_outline(text_bbox);
    REQUIRE(outline.size() == 4);
    CHECK(outline[0] == BalloonPoint{-90, -850});  // bottom-left
    CHECK(outline[1] == BalloonPoint{-90, 50});    // top-left
    CHECK(outline[2] == BalloonPoint{1290, 50});   // top-right
    CHECK(outline[3] == BalloonPoint{1290, -850}); // bottom-right
}

TEST_CASE("layout_balloon docks the cloud and points its tail at the speaker's arrowX") {
    // A compact two-line cloud that fits the panel's upper half, with the
    // speaker's body below it (speaker_top well under the cloud bottom) — the
    // real LayoutBalloons geometry.
    comicchat::FontMetrics font{};
    font.line_height = 300;
    font.base_add = 40;
    font.top_offset = 0;

    comicchat::BalloonRequest request{};
    request.kind = {comicchat::BalloonMode::say, false};
    request.text = "HELLO THERE";
    request.lines = {TextLine{"HELLO", 700}, TextLine{"THERE", 650}};
    request.font = font;
    request.arrow_x = 1500;
    request.speaker_top = -1400;
    request.place_left = 300;
    request.place_top = 0;

    const auto balloon = comicchat::layout_balloon(request);
    CHECK(balloon.has_tail);
    // DockAtTop: near-top balloon's top snaps to place_top + TOPBORDER.
    CHECK(balloon.bbox.top == comicchat::balloon_topborder);
    // The tail bottom anchors exactly at the speaker anchor (arrowX, top + 200).
    CHECK(balloon.tail.anchor == BalloonPoint{1500, -1200});
    // The outline is the closed beta-spline expansion of the placed control pts.
    CHECK(balloon.outline.size() == static_cast<std::size_t>(3 * balloon.spline.size() + 1));

    // GAP 1: the cloud is broken OPEN at the tail throat. The open outline is a
    // valid OPEN beta bezier (3*nCpsNew + 4 points for some nCpsNew), it does NOT
    // loop back to its start (front != back, unlike the closed outline), and the
    // two real wavy-bottom gap endpoints exist bracketing the break column.
    REQUIRE(balloon.outline_open.size() >= 4);
    CHECK(balloon.outline_open.front() != balloon.outline_open.back());
    CHECK((balloon.outline_open.size() - 4) % 3 == 0);
    CHECK(balloon.tail_gap_left != balloon.tail_gap_right);
    // The gap endpoints straddle the break tip (leftNearest left of rightNearest).
    CHECK(balloon.tail_gap_left.x < balloon.tail_gap_right.x);
    // GAP 2: the tail edges carry a non-zero bow altitude and a definite sign.
    CHECK(balloon.tail.altitude > 0);
    CHECK((balloon.tail.tail_sign == 1 || balloon.tail.tail_sign == -1));

    // GAP 3: a think balloon now carries BOTH the pointed tail (open cloud + arcs)
    // AND the bubble trail; a box has neither.
    comicchat::BalloonRequest think = request;
    think.kind = {comicchat::BalloonMode::think, false};
    const auto thought = comicchat::layout_balloon(think);
    CHECK(thought.has_tail);
    CHECK_FALSE(thought.bubbles.empty());
    CHECK_FALSE(thought.outline_open.empty());
    CHECK(thought.tail.altitude > 0);
    // Regression (cpp-reviewer HIGH): the think-bubble entry Y is the TEXT bbox
    // bottom (bbox.top - nLines*lineHeight - baseAdd), not the cloud route-region
    // bottom -- they differ by the AddWavies scallops, which changes bubble count
    // and spacing. Reconstruct the expected entry and confirm the trail derives
    // from it, and that the two bottoms genuinely differ so the fix matters.
    const int think_text_bottom =
        thought.bbox.top - 2 * think.font.line_height - think.font.base_add;
    CHECK(think_text_bottom != thought.route_region.bottom);
    const comicchat::BalloonPoint expect_entry{
        (thought.route_region.left + thought.route_region.right) / 2, think_text_bottom};
    const comicchat::BalloonPoint expect_tail{think.arrow_x, think.speaker_top + 200};
    CHECK(thought.bubbles == comicchat::think_bubbles(expect_entry, expect_tail));

    comicchat::BalloonRequest box = request;
    box.kind = {comicchat::BalloonMode::action, false};
    const auto action = comicchat::layout_balloon(box);
    CHECK_FALSE(action.has_tail);
    CHECK(action.outline.size() == 4);
}

namespace {

// A small known two-speaker conversation, assembled with the placement (Item
// 2.3) arrow-anchor projection feeding each balloon's tail. Deterministic: no
// TTF-dependent geometry (widths are fixed) and no RNG in drawing.
auto demo_panel() -> comicchat::Panel {
    using namespace comicchat;
    Panel panel;
    panel.seed = 0x4d2U;

    // Two bodies laid edge-to-edge; arrow_anchors gives each tail anchor x.
    const std::vector<BodySlot> slots{
        BodySlot{1, 700, 0.5, false},
        BodySlot{2, 700, 0.5, true},
    };
    const auto anchors = arrow_anchors(slots, 60, 300);

    const int body_top = -1400;
    const int body_bottom = -2240;
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        PanelBody body{};
        body.avatar_id = anchors[i].avatar_id;
        body.box = Rect{anchors[i].left, body_bottom, anchors[i].left + slots[i].width, body_top};
        body.arrow_x = anchors[i].arrow_x;
        body.color = i == 0 ? 0x6c8ebfU : 0xb85c5cU;
        panel.bodies.push_back(body);
    }

    FontMetrics font{};
    font.line_height = 300;
    font.base_add = 40;
    font.top_offset = 0;

    const auto make = [&](BalloonShapeKind kind, std::vector<TextLine> lines, int arrow_x, int place_left) {
        BalloonRequest request{};
        request.kind = kind;
        request.lines = std::move(lines);
        request.font = font;
        request.arrow_x = arrow_x;
        request.speaker_top = body_top;
        request.place_left = place_left;
        request.place_top = 0;
        return layout_balloon(request);
    };

    panel.balloons.push_back(make({BalloonMode::say, false},
                                  {TextLine{"HELLO", 700}, TextLine{"THERE", 650}}, anchors[0].arrow_x, 120));
    panel.balloons.push_back(make({BalloonMode::whisper, true}, {TextLine{"PSST", 500}},
                                  anchors[1].arrow_x, 1300));
    return panel;
}

// FNV-1a over the raw ARGB frame — a compact byte-exact signature.
auto frame_hash(const comicchat::Canvas& canvas) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto pixel : canvas.pixels()) {
        hash = (hash ^ pixel) * 1099511628211ULL;
    }
    return hash;
}

} // namespace

TEST_CASE("render_panel emits a deterministic byte-identical balloon frame") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = comicchat::TextEngine::create(*font);
    REQUIRE(text.has_value());

    const auto panel = demo_panel();

    comicchat::Canvas a{460, 460};
    a.clear({1.0, 1.0, 1.0, 1.0});
    a.render_panel(panel, **text);

    comicchat::Canvas b{460, 460};
    b.clear({1.0, 1.0, 1.0, 1.0});
    b.render_panel(panel, **text);

    // Determinism gate: identical model -> byte-identical frame (the cheapest,
    // strongest Linux test per render-port-spec.md Verification 2.1).
    CHECK(std::ranges::equal(a.pixels(), b.pixels()));
    CHECK(frame_hash(a) == frame_hash(b));

    // Structural gate: a white cloud fill over the background, black outline
    // pixels, and the two colored body placeholders are all present.
    const auto white = std::uint32_t{0xffffffffU};
    const auto black = std::uint32_t{0xff000000U};
    const auto non_white = std::ranges::count_if(a.pixels(), [white](auto p) { return p != white; });
    CHECK(non_white > 2000);
    CHECK(std::ranges::count(a.pixels(), black) > 200);  // stroked cloud + tail edges

    // Emit the golden PNG artifact for visual review (headless Cairo, no SDL).
    // Pixel-exact PNG parity vs. MSVC GDI is a visual/MSVC follow-up; the
    // deterministic hash above is the Linux gate.
    CHECK(a.write_png("balloon_panel_golden.png"));
}

TEST_CASE("render_panel draws think bubbles as width-stretched ellipses, not circles (balloon.cpp:1997-2001)") {
    // CBWoodringThink::Draw grows the bubble's circRect on WIDTH ONLY
    // (widthAdjustment applied to left/right, never top/bottom), so a bubble with
    // a non-zero width_pad must render wider than it is tall. Isolate a single
    // bubble (no cloud outline, no tail, no text) and measure the device-pixel
    // bounding box of the drawn (stroked) shape directly, rather than trusting
    // the geometry math alone.
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = comicchat::TextEngine::create(*font);
    REQUIRE(text.has_value());

    using namespace comicchat;
    Panel panel;
    Balloon balloon{};
    balloon.bubbles.push_back(ThinkBubble{BalloonPoint{1150, -1150}, 100, 100});
    panel.balloons.push_back(std::move(balloon));

    constexpr auto width = 460;
    Canvas canvas{width, width};
    canvas.clear({1.0, 1.0, 1.0, 1.0});
    canvas.render_panel(panel, **text);

    constexpr auto white = std::uint32_t{0xffffffffU};
    // Scan a window strictly inside the panel border (the border pen, kept
    // as-is by this fix, would otherwise dominate the bbox with its own square
    // footprint). The bubble center (1150,-1150) at scale 0.2 lands at device
    // (230,230); a 160x160 window around it comfortably contains the drawn
    // ellipse (x-radius/y-radius <= 40/20 device px) with margin to spare.
    constexpr auto scan_lo = 150;
    constexpr auto scan_hi = 310;
    int min_x = std::numeric_limits<int>::max();
    int max_x = std::numeric_limits<int>::min();
    int min_y = std::numeric_limits<int>::max();
    int max_y = std::numeric_limits<int>::min();
    const auto pixels = canvas.pixels();
    for (int y = scan_lo; y < scan_hi; ++y) {
        for (int x = scan_lo; x < scan_hi; ++x) {
            if (pixels[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] == white) continue;
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
        }
    }
    REQUIRE(max_x >= min_x);  // something was drawn
    REQUIRE(max_y >= min_y);
    const auto bbox_width = max_x - min_x;
    const auto bbox_height = max_y - min_y;
    CHECK(bbox_width > bbox_height);
    // radius=100, width_pad=100 -> x-radius twips is double the y-radius twips,
    // so the drawn bbox should be roughly twice as wide as it is tall (loosened
    // for AA/stroke-width slop).
    CHECK(static_cast<double>(bbox_width) > static_cast<double>(bbox_height) * 1.5);
}

// ===================================================================
// Item 2.5b#4 — render_panel blits the REAL Item 2.2 composited avatar raster
// into each placed PanelBody, replacing the flat color-box placeholder. The
// nick->avatar MAPPING is a separate 2.5b item; this task fixes the blit with a
// single hardcoded default avatar (xeno.avb, the primary complex head+torso
// composite in the shipped corpus). All geometry stays deterministic.
// ===================================================================

namespace {

// A PanelAvatarProvider backed by one default avatar's neutral pose, composited
// at the exact device pixel size render_panel asks for (crisp near-1:1 blit).
// Honors body.flip. Returns nullopt on any failure so render_panel falls back to
// the color box. Loading happens once per body render; the corpus avatar is tiny
// so this stays fast and deterministic.
auto default_avatar_provider(const char* filename) -> comicchat::PanelAvatarProvider {
    const auto path = std::filesystem::path{COMICCHAT_TEST_COMICART_DIR} / filename;
    return [path](const comicchat::PanelBody& body, std::int32_t target_width,
                  std::int32_t target_height) -> std::optional<comicchat::AvatarBitmap> {
        if (target_width <= 0 || target_height <= 0) return std::nullopt;
        const auto asset = comicchat::load_avatar_asset(path);
        if (!asset.has_value()) return std::nullopt;
        const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
        if (!neutral.has_value()) return std::nullopt;
        auto raster = comicchat::render_avatar(
            *asset, {*neutral, target_width, target_height, body.flip, false});
        if (!raster.has_value()) return std::nullopt;
        return std::move(*raster);
    };
}

// Count frame pixels exactly equal to a packed 0x00RRGGBB color, opacified to
// Cairo's post-OVER opaque ARGB (0xffRRGGBB).
auto count_color(const comicchat::Canvas& canvas, std::uint32_t rgb) -> long {
    const auto argb = 0xff000000U | (rgb & 0x00ffffffU);
    return std::ranges::count(canvas.pixels(), argb);
}

} // namespace

TEST_CASE("render_panel blits the composited avatar raster over the color-box placeholder") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = comicchat::TextEngine::create(*font);
    REQUIRE(text.has_value());

    const auto panel = demo_panel();
    REQUIRE(panel.bodies.size() == 2);
    const auto color0 = panel.bodies[0].color;  // 0x6c8ebf placeholder
    const auto color1 = panel.bodies[1].color;  // 0xb85c5c placeholder

    // Baseline: the flat color-box placeholders (no provider wired).
    comicchat::Canvas boxes{460, 460};
    boxes.clear({1.0, 1.0, 1.0, 1.0});
    boxes.render_panel(panel, **text);

    // The two body boxes are large flat fills of their placeholder colors.
    const auto box_fill0 = count_color(boxes, color0);
    const auto box_fill1 = count_color(boxes, color1);
    CHECK(box_fill0 > 3000);
    CHECK(box_fill1 > 3000);

    // Same panel, now with a real avatar composited into every body box.
    const auto provider = default_avatar_provider("xeno.avb");
    comicchat::Canvas avatars{460, 460};
    avatars.clear({1.0, 1.0, 1.0, 1.0});
    avatars.render_panel(panel, **text, provider);

    // Determinism gate: identical model + provider -> byte-identical frame.
    comicchat::Canvas avatars_repeat{460, 460};
    avatars_repeat.clear({1.0, 1.0, 1.0, 1.0});
    avatars_repeat.render_panel(panel, **text, provider);
    CHECK(std::ranges::equal(avatars.pixels(), avatars_repeat.pixels()));
    CHECK(frame_hash(avatars) == frame_hash(avatars_repeat));

    // The blit replaced the flat boxes: the placeholder-color fills are gone
    // (the opaque avatar raster overwrote the box interiors).
    const auto avatar_fill0 = count_color(avatars, color0);
    const auto avatar_fill1 = count_color(avatars, color1);
    CHECK(avatar_fill0 < box_fill0 / 4);
    CHECK(avatar_fill1 < box_fill1 / 4);

    // The avatar raster genuinely changed the body region: a large number of
    // pixels differ from the flat-box baseline, proving real pixels were drawn
    // and not a color box. (The balloons/text are identical between the two
    // frames, so every diff comes from the body boxes.)
    std::size_t differing{};
    REQUIRE(avatars.pixels().size() == boxes.pixels().size());
    for (std::size_t i = 0; i < avatars.pixels().size(); ++i) {
        if (avatars.pixels()[i] != boxes.pixels()[i]) ++differing;
    }
    CHECK(differing > 5000);

    // Region-scoped proof: inside each placed body box (the exact device rect
    // render_panel maps the logical twip box to, via the shared 2300-twip panel
    // transform) the baseline is an essentially solid placeholder-color fill,
    // while the avatar frame is dominated by real composited art — many opaque
    // pixels that are neither white nor the placeholder color. This isolates the
    // body region from the shared balloons/text, so it cleanly separates a drawn
    // avatar from a color box.
    const auto white = std::uint32_t{0xffffffffU};
    const auto transform = avatars.panel_transform();  // logical_panel_width == render_panel's 2300
    const auto pixel_at = [](const comicchat::Canvas& canvas, int x, int y) {
        return canvas.pixels()[static_cast<std::size_t>(y) * 460 + static_cast<std::size_t>(x)];
    };
    for (const auto& body : panel.bodies) {
        const auto tl = transform.to_device(comicchat::LogicalPoint{
            static_cast<double>(body.box.left), static_cast<double>(body.box.top)});
        const auto br = transform.to_device(comicchat::LogicalPoint{
            static_cast<double>(body.box.right), static_cast<double>(body.box.bottom)});
        const auto x0 = std::max(0, static_cast<int>(std::lround(tl.x)) + 1);
        const auto y0 = std::max(0, static_cast<int>(std::lround(tl.y)) + 1);
        const auto x1 = std::min(460, static_cast<int>(std::lround(br.x)) - 1);
        const auto y1 = std::min(460, static_cast<int>(std::lround(br.y)) - 1);
        REQUIRE(x1 > x0);
        REQUIRE(y1 > y0);
        const auto placeholder = 0xff000000U | (body.color & 0x00ffffffU);
        std::size_t box_placeholder{};
        std::size_t avatar_placeholder{};
        std::size_t avatar_art{};
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (pixel_at(boxes, x, y) == placeholder) ++box_placeholder;
                const auto a = pixel_at(avatars, x, y);
                if (a == placeholder) ++avatar_placeholder;
                if (a != white && a != placeholder) ++avatar_art;  // real composited silhouette
            }
        }
        const auto area = static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0);
        // Baseline body interior is (near) wholly the placeholder color.
        CHECK(box_placeholder > area * 9 / 10);
        // The flat box is gone: almost none of the placeholder color survives the
        // opaque avatar blit inside the body rect.
        CHECK(avatar_placeholder < area / 50);
        // A substantial opaque silhouette of real (non-white, non-placeholder)
        // avatar pixels now occupies the body rect — far more than the stray
        // antialiasing a color box could produce.
        CHECK(avatar_art > 800);
    }

    // Emit the headless PNG artifact showing an actual avatar in the panel.
    CHECK(avatars.write_png("avatar_panel_render.png"));
}

// ===================================================================
// Backdrop draw-hook — wires the already-tested BackdropCatalog/crop_for_panel
// (backdrop.hpp) into render_panel's draw pass, mirroring CUnitPanel::Draw's
// m_backDrop.Draw(...) call immediately after the panel clip and before any
// body draws (panel.cpp:681,684). Panel::backdrop_id defaults to nullopt
// (panel.cpp:560's id-0 "no backdrop"), so the two gates below are: (a) that
// default leaves render_panel byte-identical to before this field existed,
// and (b) a set id, resolved through a real BackdropCatalog, actually paints
// backdrop pixels into the panel interior before anything else.
// ===================================================================

TEST_CASE("render_panel is byte-identical to before when backdrop_id is unset (regression guard)") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = comicchat::TextEngine::create(*font);
    REQUIRE(text.has_value());

    const auto panel = demo_panel();
    REQUIRE_FALSE(panel.backdrop_id.has_value());  // default-constructed Panel field

    // Baseline: the original two-arg call shape (no avatars provider, no
    // backdrop catalog parameter at all -- proves the new parameter's default
    // doesn't perturb existing call sites).
    comicchat::Canvas baseline{460, 460};
    baseline.clear({1.0, 1.0, 1.0, 1.0});
    baseline.render_panel(panel, **text);

    // Same panel, now explicitly passing a real, populated BackdropCatalog --
    // but backdrop_id stays unset, so the backdrop pass must not run.
    comicchat::BackdropCatalog catalog{std::filesystem::path{COMICCHAT_TEST_COMICART_DIR}};
    comicchat::Canvas with_catalog{460, 460};
    with_catalog.clear({1.0, 1.0, 1.0, 1.0});
    with_catalog.render_panel(panel, **text, {}, &catalog);
    CHECK(std::ranges::equal(baseline.pixels(), with_catalog.pixels()));
    CHECK(frame_hash(baseline) == frame_hash(with_catalog));

    // A panel that DOES set backdrop_id but is rendered with no catalog
    // (nullptr, the default) must also fall back to the unchanged baseline --
    // there is nothing to resolve the id against.
    auto panel_with_id = panel;
    panel_with_id.backdrop_id = catalog.id_for_name("room.bgb");
    REQUIRE(*panel_with_id.backdrop_id != 0);
    comicchat::Canvas no_catalog{460, 460};
    no_catalog.clear({1.0, 1.0, 1.0, 1.0});
    no_catalog.render_panel(panel_with_id, **text);
    CHECK(std::ranges::equal(baseline.pixels(), no_catalog.pixels()));
    CHECK(frame_hash(baseline) == frame_hash(no_catalog));
}

TEST_CASE("render_panel draws backdrop art behind an otherwise-blank panel") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = comicchat::TextEngine::create(*font);
    REQUIRE(text.has_value());

    comicchat::BackdropCatalog catalog{std::filesystem::path{COMICCHAT_TEST_COMICART_DIR}};
    const auto backdrop_id = catalog.id_for_name("room.bgb");
    REQUIRE(backdrop_id != 0);

    constexpr auto width = 460;
    const auto white = std::uint32_t{0xffffffffU};

    // An empty panel (no bodies, no balloons): every interior pixel is
    // background-only, so any ink there can only have come from the backdrop
    // pass (or the fixed border stroke, which the sample point below avoids).
    comicchat::Panel panel;

    comicchat::Canvas without{width, width};
    without.clear({1.0, 1.0, 1.0, 1.0});
    without.render_panel(panel, **text, {}, &catalog);  // backdrop_id unset: no backdrop pass

    panel.backdrop_id = backdrop_id;
    comicchat::Canvas with{width, width};
    with.clear({1.0, 1.0, 1.0, 1.0});
    with.render_panel(panel, **text, {}, &catalog);

    // Grid-sample the panel interior, well inside the fixed border stroke
    // (~24 device px wide at this scale) so every sample point is a pure
    // background-only pixel with no bodies/balloons in play. Without a
    // backdrop every one of these must be blank white (proving the region
    // really is "otherwise blank"); with a backdrop, room.bgb's real scene
    // content (see backdrop_panel_render.png) is not uniformly white -- it has
    // a hatched border pattern and shaded foreground -- so a solid majority of
    // the same sample points must turn non-white ink from the backdrop blit.
    const auto pixel_at = [](const comicchat::Canvas& canvas, const int x, const int y) {
        return canvas.pixels()[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)];
    };
    int interior_samples = 0;
    int without_non_white = 0;
    int with_non_white = 0;
    for (int y = 40; y < width - 40; y += 20) {
        for (int x = 40; x < width - 40; x += 20) {
            ++interior_samples;
            if (pixel_at(without, x, y) != white) ++without_non_white;
            if (pixel_at(with, x, y) != white) ++with_non_white;
        }
    }
    REQUIRE(interior_samples > 0);
    CHECK(without_non_white == 0);                        // otherwise blank without a backdrop
    CHECK(with_non_white > interior_samples / 2);          // real backdrop ink now dominates

    // Broader gate: a real scene bitmap stretched to fill the whole panel
    // interior should paint far more than a handful of pixels.
    const auto non_white_without =
        std::ranges::count_if(without.pixels(), [white](const auto pixel) { return pixel != white; });
    const auto non_white_with =
        std::ranges::count_if(with.pixels(), [white](const auto pixel) { return pixel != white; });
    CHECK(non_white_with > non_white_without + 10'000);

    // Determinism: the same Panel + catalog yields a byte-identical frame.
    comicchat::Canvas with_repeat{width, width};
    with_repeat.clear({1.0, 1.0, 1.0, 1.0});
    with_repeat.render_panel(panel, **text, {}, &catalog);
    CHECK(std::ranges::equal(with.pixels(), with_repeat.pixels()));
    CHECK(frame_hash(with) == frame_hash(with_repeat));

    CHECK(with.write_png("backdrop_panel_render.png"));
}

// ===================================================================
// Render fidelity — the balloon text must FIT inside the cloud outline. The
// cloud is sized by measuring the wrapped lines at message_text_size, so the
// drawn text (threaded balloon.text_size == message_text_size) must land inside
// the drawn beta-spline outline. This is the regression gate for the "text
// overflows the balloon" defect (render.cpp used to draw at line_height*0.72).
// ===================================================================

namespace {

// The horizontal span [min_x, max_x] of a balloon's drawn beta-spline outline,
// in panel twips -- the true visual edge the text must sit inside of.
auto outline_x_span(const comicchat::Balloon& balloon) -> std::pair<int, int> {
    int min_x = std::numeric_limits<int>::max();
    int max_x = std::numeric_limits<int>::min();
    for (const auto& point : balloon.outline) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
    }
    return {min_x, max_x};
}

// Assert every wrapped line, drawn centered at the cloud center at the threaded
// text pixel size, stays strictly inside the cloud outline. Uses the SAME engine
// to measure that render_panel draws with, so measured == drawn width.
void check_text_fits_cloud(comicchat::TextEngine& engine, const comicchat::Balloon& balloon) {
    REQUIRE_FALSE(balloon.lines.empty());
    REQUIRE(balloon.text_size > 0.0);
    const auto [min_x, max_x] = outline_x_span(balloon);
    // Mirror render.cpp: each line is drawn left-anchored at bbox.left + the
    // ShiftLines offset (maxWidth - width_i)/2 (center-justified say/whisper/think).
    const bool left_justify = balloon.kind.mode == comicchat::BalloonMode::action;
    const int max_line_width = comicchat::widest_line_width(balloon.lines);
    for (const auto& line : balloon.lines) {
        if (line.text.empty()) continue;
        const auto width = engine.measure_width(line.text, balloon.text_size);
        REQUIRE(width.has_value());
        const int offset = left_justify ? 0 : (max_line_width - line.width) / 2;
        const int left_x = balloon.bbox.left + offset;
        // The drawn ink span [left_x, left_x + width] lies inside the outline.
        CHECK(left_x >= min_x);
        CHECK(left_x + *width <= max_x);
    }
}

} // namespace

TEST_CASE("balloon text stays inside the cloud outline (no overflow)") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = comicchat::TextEngine::create(*font);
    REQUIRE(text.has_value());

    // The exact --say line the visual gate renders, plus a long multi-line case.
    for (const std::string_view message : {"hello comic world",
                                           "the quick brown fox jumps over the lazy dog again",
                                           "SHORT"}) {
        const auto panel = comicchat::build_say_panel(**text, "Ada", message);
        REQUIRE(panel.balloons.size() == 1);
        check_text_fits_cloud(**text, panel.balloons.front());
    }
}

TEST_CASE("open cloud and text rasterize inside a say balloon") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = comicchat::TextEngine::create(*font);
    REQUIRE(text.has_value());

    const auto metrics = comicchat::build_say_font_metrics(**text);
    REQUIRE(metrics.has_value());
    comicchat::PageConfig config;
    config.font = *metrics;
    config.text_size = comicchat::message_text_size;
    config.max_text_width = comicchat::message_balloon_max_width;
    comicchat::Page page{config, comicchat::measure_text_width(**text, comicchat::message_text_size)};
    comicchat::PageAvatar speaker;
    speaker.avatar_id = comicchat::nick_avatar_id("Ada");
    speaker.body_width = comicchat::message_body_width;
    speaker.body_height = comicchat::message_body_height;
    speaker.face_fraction = 0.5;
    speaker.color = comicchat::nick_color("Ada");
    page.add_line(comicchat::Line{speaker, "hello comic world", comicchat::bm_say});
    REQUIRE_FALSE(page.panels().empty());
    CHECK(page.panels().size() == 1);
    REQUIRE_FALSE(page.panels().front().balloons.empty());
    CAPTURE(page.panels().size(), page.panels().front().balloons.front().text,
            page.panels().front().balloons.front().lines.size());
    const auto& panel = page.panels().back();
    REQUIRE(panel.balloons.size() == 1);
    const auto& balloon = panel.balloons.front();
    REQUIRE_FALSE(balloon.lines.empty());
    REQUIRE(balloon.outline_open.size() >= 4);
    CHECK(balloon.tail.altitude > 0);

    // BreakSpline replaces only the short tail-throat run. Its OPEN spline must
    // still traverse the full cloud and terminate at the two real gap points.
    CHECK(balloon.outline_open.front() == balloon.tail_gap_right);
    CHECK(balloon.outline_open.back() == balloon.tail_gap_left);
    const auto [closed_min_x, closed_max_x] = outline_x_span(balloon);
    const auto [open_min_it, open_max_it] = std::ranges::minmax_element(
        balloon.outline_open, {}, &comicchat::BalloonPoint::x);
    REQUIRE(open_min_it != balloon.outline_open.end());
    REQUIRE(open_max_it != balloon.outline_open.end());
    CAPTURE(balloon.lines.size(), balloon.bbox.left, balloon.bbox.right, balloon.bbox.top,
            balloon.bbox.bottom, balloon.route_region.left, balloon.route_region.right,
            balloon.route_region.top, balloon.route_region.bottom, balloon.line_height,
            balloon.text_size, balloon.lines.front().text, balloon.lines.front().width,
            closed_min_x, closed_max_x, open_min_it->x, open_max_it->x);
    CHECK(open_min_it->x <= closed_min_x + comicchat::balloon_xborder);
    CHECK(open_max_it->x >= closed_max_x - comicchat::balloon_xborder);

    comicchat::Canvas canvas{760, 760};
    canvas.clear({1.0, 1.0, 1.0, 1.0});
    canvas.render_panel(panel, **text);

    // The text is drawn inside this source text cell, away from the cloud edge
    // and tail. Dark pixels here therefore prove that the Cairo path pass left a
    // usable context and DrawText reached the raster, not merely that the avatar
    // or panel border contributed ink elsewhere in the frame.
    const auto transform = canvas.panel_transform();
    const auto gap_left = transform.to_device(comicchat::LogicalPoint{
        static_cast<double>(balloon.tail_gap_left.x), static_cast<double>(balloon.tail_gap_left.y)});
    const auto gap_right = transform.to_device(comicchat::LogicalPoint{
        static_cast<double>(balloon.tail_gap_right.x), static_cast<double>(balloon.tail_gap_right.y)});
    std::size_t dark_throat_samples{};
    for (int step = 4; step <= 16; ++step) {
        const double alpha = static_cast<double>(step) / 20.0;
        const auto x = std::clamp(static_cast<int>(std::lround(
                                      gap_left.x + alpha * (gap_right.x - gap_left.x))),
                                  1, 758);
        const auto y = std::clamp(static_cast<int>(std::lround(
                                      gap_left.y + alpha * (gap_right.y - gap_left.y))),
                                  1, 758);
        bool dark_sample = false;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto pixel = canvas.pixels()[static_cast<std::size_t>(y + dy) * 760U +
                                                   static_cast<std::size_t>(x + dx)];
                const auto red = (pixel >> 16U) & 0xffU;
                const auto green = (pixel >> 8U) & 0xffU;
                const auto blue = pixel & 0xffU;
                dark_sample = dark_sample || (red < 96U && green < 96U && blue < 96U);
            }
        }
        if (dark_sample) ++dark_throat_samples;
    }
    // A reverted closed cloud would stroke a black line across nearly every
    // sample. The open cloud plus bowed arcs leaves the throat interior white.
    CHECK(dark_throat_samples <= 2);

    const auto text_top_left = transform.to_device(comicchat::LogicalPoint{
        static_cast<double>(balloon.bbox.left), static_cast<double>(balloon.bbox.top)});
    const auto text_bottom_right = transform.to_device(comicchat::LogicalPoint{
        static_cast<double>(balloon.bbox.left + comicchat::widest_line_width(balloon.lines)),
        static_cast<double>(balloon.bbox.top - balloon.line_height * static_cast<int>(balloon.lines.size()))});
    const auto x0 = std::clamp(static_cast<int>(std::floor(text_top_left.x)), 0, 759);
    const auto y0 = std::clamp(static_cast<int>(std::floor(text_top_left.y)), 0, 759);
    const auto x1 = std::clamp(static_cast<int>(std::ceil(text_bottom_right.x)), x0 + 1, 760);
    const auto y1 = std::clamp(static_cast<int>(std::ceil(text_bottom_right.y)), y0 + 1, 760);
    std::size_t dark_text_pixels{};
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const auto pixel = canvas.pixels()[static_cast<std::size_t>(y) * 760U + static_cast<std::size_t>(x)];
            const auto red = (pixel >> 16U) & 0xffU;
            const auto green = (pixel >> 8U) & 0xffU;
            const auto blue = pixel & 0xffU;
            if (red < 96U && green < 96U && blue < 96U) ++dark_text_pixels;
        }
    }
    CHECK(dark_text_pixels > 100);
}

TEST_CASE("page-composed balloon text stays inside the cloud outline") {
    using namespace comicchat;
    const auto font = find_portable_comic_font();
    REQUIRE(font.has_value());
    auto text = TextEngine::create(*font);
    REQUIRE(text.has_value());

    const auto metrics = build_font_metrics(**text, message_text_size, 0, 0);
    REQUIRE(metrics.has_value());
    PageConfig cfg;
    cfg.font = *metrics;
    cfg.text_size = message_text_size;
    cfg.max_text_width = message_balloon_max_width;
    Page page{cfg, measure_text_width(**text, message_text_size)};

    PageAvatar ada;
    ada.avatar_id = nick_avatar_id("Ada");
    ada.color = nick_color("Ada");
    page.add_line(Line{ada, "hello comic world", bm_say});

    REQUIRE_FALSE(page.panels().empty());
    const auto& panel = page.panels().back();
    REQUIRE_FALSE(panel.balloons.empty());
    for (const auto& balloon : panel.balloons) {
        check_text_fits_cloud(**text, balloon);
    }
}
