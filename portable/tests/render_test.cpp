#include "comicchat/balloon.hpp"
#include "comicchat/layout.hpp"
#include "comicchat/render.hpp"
#include "comicchat/text.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
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

    // A think balloon replaces the tail with a bubble trail; a box has neither.
    comicchat::BalloonRequest think = request;
    think.kind = {comicchat::BalloonMode::think, false};
    const auto thought = comicchat::layout_balloon(think);
    CHECK_FALSE(thought.has_tail);
    CHECK_FALSE(thought.bubbles.empty());
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
