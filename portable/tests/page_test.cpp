#include "comicchat/balloon.hpp"
#include "comicchat/layout.hpp"
#include "comicchat/page.hpp"
#include "comicchat/render.hpp"
#include "comicchat/text.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace {

using namespace comicchat;

// A fixed-advance measure so line breaks and widths are arithmetic and the
// layout is TTF-independent: every byte is `advance` twips wide.
auto monospace_measure(std::int32_t advance) -> TextMeasure {
    return [advance](std::string_view text) -> std::int32_t {
        return static_cast<std::int32_t>(text.size()) * advance;
    };
}

// The say-font metrics used across the deterministic geometry tests (mirrors
// comic_page_test's synthetic font).
auto say_font() -> FontMetrics {
    FontMetrics font{};
    font.line_height = 300;
    font.base_add = 40;
    font.top_offset = 0;
    return font;
}

auto config() -> PageConfig {
    PageConfig cfg{};
    cfg.font = say_font();
    cfg.max_text_width = 1200;
    cfg.seed = 12345U;
    return cfg;
}

// A speaker with a fixed 400x800 body, no talk-tos.
auto speaker(std::uint32_t id) -> PageAvatar {
    PageAvatar sel{};
    sel.avatar_id = id;
    sel.body_width = 400;
    sel.body_height = 800;
    sel.face_fraction = 0.5;
    sel.color = 0x445566U + id;
    return sel;
}

auto say_line(std::uint32_t id, std::string_view text) -> Line {
    return Line{speaker(id), std::string{text}, bm_say};
}

// ---------------------------------------------------------------------------
// Independent PRNG oracle: a standalone reimplementation of page.cpp's
// LayoutBalloons balloon loop (panel.cpp:858) built ONLY from the public ported
// primitives (cloud_estimate, layout_balloon, GetInterveningBBox / QueryRouteRgn
// / SetRouteRgn ported below). It threads the panel PRNG in the exact Microsoft
// order and optionally drops the ShiftLines draws, so a test can prove the page
// matches the correctly-threaded stream and diverges from the dropped one.
// ---------------------------------------------------------------------------
constexpr int kLargeInteger = 100000000;
constexpr int kDockDelta = balloon_topborder + balloon_yborder + balloon_hwave_height;  // 90

struct OraclePlaced {
    int arrow_x{};
    Rect cloud_bbox{};
    int route_left{};
    int route_right{};
    int bbox_bottom{};
};

void oracle_query(const OraclePlaced& b, int other_to_x, int& la, int& ra) {
    const int to_x = b.arrow_x;
    if (other_to_x > to_x) {
        la = std::max(to_x, b.route_left + balloon_min_route_width);
        ra = kLargeInteger;
    } else {
        la = -kLargeInteger;
        ra = std::min(to_x, b.route_right - balloon_min_route_width);
    }
}

void oracle_set(OraclePlaced& b, int other_to_x, int left, int right) {
    if (other_to_x > b.arrow_x) {
        b.route_right = std::min(b.route_right, left);
    } else {
        b.route_left = std::max(b.route_left, right);
    }
}

void oracle_intervening(const std::vector<OraclePlaced>& placed, const Rect& free_rect, Rect& irect,
                        int arrow_x) {
    int most_left = free_rect.left;
    int most_right = free_rect.right;
    for (const auto& b : placed) {
        int la = 0;
        int ra = 0;
        oracle_query(b, arrow_x, la, ra);
        most_left = std::max(la, most_left);
        most_right = std::min(ra, most_right);
    }
    if (most_left > irect.left || most_right < irect.right) {
        const int clearance = most_right - most_left;
        if (clearance >= (irect.right - irect.left)) {
            const int delta = most_left > irect.left ? most_left - irect.left : most_right - irect.right;
            irect.left += delta;
            irect.right += delta;
        } else {
            irect.left = most_left;
            irect.right = most_right;
        }
    }
    irect.top = free_rect.top;
    for (const auto& b : placed) {
        Rect cb = b.cloud_bbox;
        if (cb.right < irect.left) {
            irect.top = std::min(irect.top, cb.top);
        } else {
            cb.top += kDockDelta;
            cb.bottom += kDockDelta;
            irect.top = std::min(irect.top, cb.bottom);
        }
    }
}

struct OracleLine {
    std::string text;
    std::uint16_t modes{bm_say};
    int arrow_x{};
    int speaker_top{};
};

auto oracle_layout(std::uint32_t seed, const Rect& free_rect, const FontMetrics& font,
                   const TextMeasure& measure, std::int32_t max_width,
                   const std::vector<OracleLine>& lines, bool thread_shift) -> std::vector<Balloon> {
    MsvcrtRandom rng{seed};
    std::vector<OraclePlaced> placed;
    std::vector<Balloon> out;
    for (const auto& l : lines) {
        const auto kind = select_balloon_mode(l.modes);
        const bool is_box = kind.mode == BalloonMode::action;
        const auto wrapped = break_into_lines(measure, max_width, l.text);

        CloudEstimateInput in{};
        in.text_extent = measure(l.text);
        in.text_height = font.line_height;
        in.line_height = font.line_height;
        // WidestWord (single word here) approximated by whole-text extent for the
        // oracle scenario, which uses single-word or short texts.
        in.widest_word = measure(l.text);
        in.free_left = free_rect.left;
        in.free_right = free_rect.right;
        in.free_top = free_rect.top;
        in.free_bottom = free_rect.bottom;
        int lowest = free_rect.top;
        for (const auto& p : placed) {
            lowest = std::min(lowest, p.bbox_bottom);
        }
        in.lowest_prev_bottom = lowest;
        in.arrow_x = l.arrow_x;
        in.is_box = is_box;

        const auto est = cloud_estimate(rng, in);
        Rect brect{est.left, 0, est.right, free_rect.top};
        oracle_intervening(placed, free_rect, brect, l.arrow_x);

        BalloonRequest req{};
        req.kind = kind;
        req.text = l.text;
        req.lines = wrapped;
        req.font = font;
        req.arrow_x = l.arrow_x;
        req.speaker_top = l.speaker_top;
        req.place_left = brect.left;
        req.place_top = brect.top;
        Balloon b = layout_balloon(req);

        if (thread_shift) {
            for (std::size_t k = 0; k < b.lines.size(); ++k) {
                (void)rng.next();
            }
        }

        OraclePlaced rec{};
        rec.arrow_x = l.arrow_x;
        rec.cloud_bbox = b.route_region;
        rec.route_left = b.route_region.left;
        rec.route_right = b.route_region.right;
        rec.bbox_bottom = b.bbox.bottom;
        for (auto& p : placed) {
            oracle_set(p, l.arrow_x, rec.route_left, rec.route_right);
        }
        placed.push_back(rec);
        out.push_back(std::move(b));
    }
    return out;
}

// Rebuild the OracleLine list for a rendered panel using its verified placement
// (arrow_x + body top read back from panel.bodies).
auto oracle_lines_for(const Panel& panel, const std::vector<Line>& speak_lines) -> std::vector<OracleLine> {
    std::vector<OracleLine> out;
    for (const auto& line : speak_lines) {
        const auto body = std::find_if(panel.bodies.begin(), panel.bodies.end(),
                                       [&](const PanelBody& b) {
                                           return b.avatar_id == line.speaker.avatar_id;
                                       });
        REQUIRE(body != panel.bodies.end());
        out.push_back(OracleLine{line.text, line.modes, body->arrow_x, body->box.top});
    }
    return out;
}

} // namespace

// ===========================================================================
// Panel split points (panel.cpp:1082): the first two lines always open new
// panels (m_panels.GetCount() < 2), then further speakers accrete into the tail.
// ===========================================================================
TEST_CASE("page splits the first two lines into separate panels, then accretes") {
    Page page{config(), monospace_measure(100)};
    page.add_line(say_line(1, "aa"));
    page.add_line(say_line(2, "bb"));
    page.add_line(say_line(3, "cc"));
    page.add_line(say_line(4, "dd"));

    // Line 1 -> panel 0 (new_panel_pending). Line 2 -> panel 1 (panel_count < 2).
    // Lines 3 and 4 clone panel 1 (distinct speakers, < 5 elements).
    REQUIRE(page.panels().size() == 2);
    CHECK(page.panels()[0].balloons.size() == 1);
    CHECK(page.panels()[0].bodies.size() == 1);
    CHECK(page.panels()[1].balloons.size() == 3);
    CHECK(page.panels()[1].bodies.size() == 3);

    // The tail panel's speak order is the element (first-appearance) order.
    CHECK(page.panel_infos()[1].speak_order == std::vector<std::uint32_t>{2, 3, 4});
}

// A repeat speaker forces a new panel (AvatarInPanel, panel.cpp:1082).
TEST_CASE("a repeat speaker opens a fresh panel") {
    Page page{config(), monospace_measure(100)};
    page.add_line(say_line(1, "aa"));
    page.add_line(say_line(2, "bb"));
    page.add_line(say_line(3, "cc"));   // panel 1 = {2,3}
    page.add_line(say_line(2, "dd"));   // 2 already in panel 1 -> new panel 2

    REQUIRE(page.panels().size() == 3);
    CHECK(page.panels()[1].balloons.size() == 2);
    CHECK(page.panels()[2].balloons.size() == 1);
    CHECK(page.panel_infos()[2].speak_order == std::vector<std::uint32_t>{2});
}

// A five-element tail panel is full; the sixth speaker starts a new panel.
TEST_CASE("the sixth speaker starts a new panel once the tail holds five") {
    Page page{config(), monospace_measure(100)};
    for (std::uint32_t id = 1; id <= 7; ++id) {
        page.add_line(say_line(id, "hi"));
    }
    // 1 -> p0, 2 -> p1, then 3,4,5,6 accrete into p1 (5 elements), 7 -> p2.
    REQUIRE(page.panels().size() == 3);
    CHECK(page.panels()[1].balloons.size() == 5);
    CHECK(page.panels()[2].balloons.size() == 1);
}

// ===========================================================================
// Multi-body ordering: the page must feed order_conversation the element-order
// speakers and place bodies in the greedy left-to-right result.
// ===========================================================================
TEST_CASE("page body order matches order_conversation") {
    Page page{config(), monospace_measure(100)};
    page.add_line(say_line(10, "aa"));   // panel 0
    page.add_line(say_line(20, "bb"));   // panel 1
    page.add_line(say_line(30, "cc"));   // panel 1 gains 30
    page.add_line(say_line(40, "dd"));   // panel 1 gains 40

    const auto& tail = page.panels().back();
    const auto& tail_info = page.panel_infos().back();

    // Independent order_conversation over the same speakers/registry/historesis.
    // Panel 1 was opened at line 20; its historesis_in is panel 0's output
    // (placing only avatar 10), which does not touch 20/30/40.
    std::vector<ConversationAvatar> avatars{{10, {}}, {20, {}}, {30, {}}, {40, {}}};
    HistoresisMap hist_after_panel0 =
        order_conversation({10}, avatars, HistoresisMap{}).historesis;
    const auto expected = order_conversation({20, 30, 40}, avatars, hist_after_panel0);

    std::vector<std::uint32_t> expected_order;
    for (const auto& b : expected.bodies) {
        expected_order.push_back(b.avatar_id);
    }
    CHECK(tail_info.body_order == expected_order);
    REQUIRE(tail.bodies.size() == expected.bodies.size());
    for (std::size_t i = 0; i < tail.bodies.size(); ++i) {
        CHECK(tail.bodies[i].avatar_id == expected.bodies[i].avatar_id);
        CHECK(tail.bodies[i].flip == expected.bodies[i].flip);
    }
    // Bodies are laid edge-to-edge left to right with a positive margin.
    for (std::size_t i = 1; i < tail.bodies.size(); ++i) {
        CHECK(tail.bodies[i].box.left > tail.bodies[i - 1].box.left);
    }
}

// A talk-to partner is pulled into the panel as an extra body without a balloon
// (order_conversation AddTalkTos, panel.cpp:317).
TEST_CASE("a talk-to partner is pulled in as a bodiless-balloon avatar") {
    Page page{config(), monospace_measure(100)};
    PageAvatar a = speaker(100);
    PageAvatar b = speaker(200);
    a.talk_tos = {200};  // 100 addresses 200
    page.add_participant(b);  // 200 is a channel participant that never speaks

    page.add_line(say_line(1, "x"));                  // panel 0 (unrelated)
    page.add_line(Line{a, "hey 200", bm_say});        // panel 1: speaker 100 pulls 200

    const auto& tail = page.panels().back();
    // Two bodies (100 + pulled 200) but only one balloon (100 spoke).
    CHECK(tail.bodies.size() == 2);
    CHECK(tail.balloons.size() == 1);
    const bool has200 = std::any_of(tail.bodies.begin(), tail.bodies.end(),
                                    [](const PanelBody& body) { return body.avatar_id == 200; });
    CHECK(has200);
}

// ===========================================================================
// LayoutAvatars body scaling (Item 2.2, panel.cpp:740,759-819): placed bodies
// normalize onto a shared maxBodyHeight instead of rendering at their raw
// pre-fitted dimensions.
// ===========================================================================
TEST_CASE("a lone body normalizes to maxBodyHeight, not its raw body_height") {
    // Disable the zoom-in branch so this isolates the maxBodyHeight
    // normalization step (panel.cpp:759-775) from the zoom step (panel.cpp:
    // 791-806) that would otherwise also scale a lone (non-overflowing) body.
    auto cfg = config();
    cfg.zoom_avatars = false;

    Page page{cfg, monospace_measure(100)};
    page.add_line(say_line(1, "hi"));  // panel 0: a single 400x800 body.

    const auto& panel = page.panels().front();
    REQUIRE(panel.bodies.size() == 1);
    const auto& body = panel.bodies.front();

    // maxBodyHeight = (int)(m_unitHeight / 1.9) = (int)(2300 / 1.9) = 1210
    // (panel.cpp:740). A lone body's normHeight defaults to its own
    // body_height, so maxNorm == normHeight and the body normalizes to
    // exactly maxBodyHeight, not its raw 800.
    constexpr std::int32_t expected_height = 1210;
    CHECK(body.box.top - body.box.bottom == expected_height);
    CHECK(body.box.bottom == -cfg.unit_height);  // SetBBox floor (panel.cpp:816).

    // width scales by the same ratio: scaleRatio = 1210/800 = 1.5125,
    // ROUND(400 * 1.5125) = 605 (panel.cpp:771).
    constexpr std::int32_t expected_width = 605;
    CHECK(body.box.right - body.box.left == expected_width);
}

// The zoom-in branch (panel.cpp:791-806) fills unused panel width once bodies
// no longer overflow it, capped by the "don't cut at neck" head factor.
TEST_CASE("a lone narrow body zooms in to fill unused panel width") {
    Page page{config(), monospace_measure(100)};  // zoom_avatars defaults true.
    page.add_line(say_line(1, "hi"));

    const auto& body = page.panels().front().bodies.front();
    const auto height = body.box.top - body.box.bottom;
    const auto width = body.box.right - body.box.left;

    // Normalized-only geometry (see the disabled-zoom case above) would be
    // 1210x605; zooming a lone narrow body up to fill the 2300-wide panel
    // must make it taller and wider than that.
    CHECK(height > 1210);
    CHECK(width > 605);
    // Head-anchored (panel.cpp:816): the zoom does NOT recompute top[i], so the
    // head stays pinned at -unitHeight + the normalized 1210, ON panel, while the
    // feet sink below the floor (clipped) — the "balloon covers the avatar" fix.
    CHECK(body.box.top == -config().unit_height + 1210);
    CHECK(body.box.top <= 0);                          // head never above panel top.
    CHECK(body.box.bottom < -config().unit_height);    // feet clipped below floor.
}

// Multiple bodies whose normalized+zoomed widths sum past the panel width
// still place left-to-right with a (possibly zero, never negative) margin.
TEST_CASE("scaled multi-body placement keeps left-to-right order and floor") {
    Page page{config(), monospace_measure(100)};
    page.add_line(say_line(1, "aa"));
    page.add_line(say_line(2, "bb"));
    page.add_line(say_line(3, "cc"));
    page.add_line(say_line(4, "dd"));

    const auto& tail = page.panels().back();
    REQUIRE(tail.bodies.size() >= 2);
    for (const auto& body : tail.bodies) {
        CHECK(body.box.top <= 0);                        // head on-panel (head-anchored).
        CHECK(body.box.bottom <= -config().unit_height); // feet on or below the floor.
        CHECK(body.box.top > body.box.bottom);
        CHECK(body.box.right > body.box.left);
    }
    for (std::size_t i = 1; i < tail.bodies.size(); ++i) {
        CHECK(tail.bodies[i].box.left > tail.bodies[i - 1].box.left);
    }
}

// ===========================================================================
// PRNG threading (the 2.1 MEDIUM): the panel PRNG must be advanced by the
// ShiftLines per-line draws after each balloon, in Microsoft rand() order.
// ===========================================================================
TEST_CASE("multi-balloon panel matches the correctly-threaded PRNG oracle") {
    Page page{config(), monospace_measure(100)};
    // Short single-line balloons that all fit: p0={1}, p1={2,3,4}. Even with one
    // line each, threading the ShiftLines draw after every balloon shifts the
    // stream the next balloon's GetCloudEstimate reads.
    const std::vector<Line> lines{
        say_line(1, "hi"),
        say_line(2, "yo"),
        say_line(3, "ok"),
        say_line(4, "no"),
    };
    for (const auto& l : lines) {
        page.add_line(l);
    }

    REQUIRE(page.panels().size() == 2);
    const auto& tail = page.panels()[1];
    REQUIRE(tail.balloons.size() == 3);

    const std::vector<Line> speak{lines[1], lines[2], lines[3]};
    const auto oracle_lines = oracle_lines_for(tail, speak);

    const auto cfg = config();
    Rect free_rect{0, -cfg.unit_height / 2, cfg.unit_width, 0};
    free_rect.left += cfg.border_width;
    free_rect.right -= cfg.border_width;
    free_rect.top -= cfg.border_width;

    const std::uint32_t seed = page.panel_infos()[1].seed;
    const auto threaded =
        oracle_layout(seed, free_rect, cfg.font, monospace_measure(100), cfg.max_text_width,
                      oracle_lines, /*thread_shift=*/true);
    const auto dropped =
        oracle_layout(seed, free_rect, cfg.font, monospace_measure(100), cfg.max_text_width,
                      oracle_lines, /*thread_shift=*/false);

    REQUIRE(threaded.size() == tail.balloons.size());
    for (std::size_t i = 0; i < threaded.size(); ++i) {
        CHECK(tail.balloons[i].bbox == threaded[i].bbox);
        CHECK(tail.balloons[i].tail == threaded[i].tail);
        CHECK(tail.balloons[i].outline == threaded[i].outline);
    }

    // The correctly-threaded stream must differ from the ShiftLines-dropped one
    // on a later balloon: this is exactly the desync the fix closes.
    bool diverged = false;
    for (std::size_t i = 0; i < threaded.size(); ++i) {
        if (!(threaded[i].bbox == dropped[i].bbox)) {
            diverged = true;
        }
    }
    CHECK(diverged);
}

// ===========================================================================
// Determinism: the same line sequence and seed reproduce byte-identical panels.
// ===========================================================================
TEST_CASE("page composition is deterministic under a fixed seed") {
    const auto build = [] {
        Page page{config(), monospace_measure(100)};
        page.add_line(say_line(1, "alpha"));
        page.add_line(say_line(2, "beta gamma"));
        page.add_line(say_line(3, "delta epsilon zeta"));
        page.add_line(say_line(4, "eta"));
        return page;
    };
    const auto a = build();
    const auto b = build();
    REQUIRE(a.panels().size() == b.panels().size());
    for (std::size_t p = 0; p < a.panels().size(); ++p) {
        REQUIRE(a.panels()[p].balloons.size() == b.panels()[p].balloons.size());
        CHECK(a.panels()[p].seed == b.panels()[p].seed);
        for (std::size_t i = 0; i < a.panels()[p].balloons.size(); ++i) {
            CHECK(a.panels()[p].balloons[i].bbox == b.panels()[p].balloons[i].bbox);
            CHECK(a.panels()[p].balloons[i].outline == b.panels()[p].balloons[i].outline);
        }
        for (std::size_t i = 0; i < a.panels()[p].bodies.size(); ++i) {
            CHECK(a.panels()[p].bodies[i].box == b.panels()[p].bodies[i].box);
        }
    }
}

// ===========================================================================
// Headless render: the page must be drawable to a PNG through the real renderer.
// ===========================================================================
TEST_CASE("a composed page renders to a PNG headlessly") {
    const auto font_path = find_portable_comic_font();
    REQUIRE(font_path.has_value());
    auto engine = TextEngine::create(*font_path);
    REQUIRE(engine.has_value());

    constexpr double text_size = 220.0;
    PageConfig cfg{};
    const auto metrics = build_font_metrics(**engine, text_size, 0, 0);
    REQUIRE(metrics.has_value());
    cfg.font = *metrics;
    cfg.max_text_width = page_default_max_text_width;

    Page page{cfg, measure_text_width(**engine, text_size)};
    page.add_line(say_line(1, "Hello from the page layer!"));
    page.add_line(say_line(2, "Multiple speakers now share panels."));
    page.add_line(say_line(3, "And the balloons route around each other."));
    REQUIRE(!page.panels().empty());

    Canvas canvas{640, 640};
    canvas.clear(Rgba{1.0, 1.0, 1.0, 1.0});
    canvas.render_panel(page.panels().back(), **engine);

    const auto out = std::filesystem::temp_directory_path() / "comicchat_page_test.png";
    CHECK(canvas.write_png(out.string()));
    std::error_code ec;
    CHECK(std::filesystem::file_size(out, ec) > 0);
    std::filesystem::remove(out, ec);
}
