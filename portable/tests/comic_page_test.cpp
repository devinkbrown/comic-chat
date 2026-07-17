#include "comicchat/balloon.hpp"
#include "comicchat/comic_page.hpp"
#include "comicchat/layout.hpp"
#include "comicchat/text.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

namespace {

// A synthetic fixed-advance measure so the wrap/layout is TTF-independent and
// the expected line breaks are arithmetic: every glyph is `advance` twips wide.
auto monospace_measure(std::int32_t advance) -> comicchat::TextMeasure {
    return [advance](std::string_view text) -> std::int32_t {
        return static_cast<std::int32_t>(text.size()) * advance;
    };
}

auto say_font() -> comicchat::FontMetrics {
    comicchat::FontMetrics font{};
    font.line_height = 300;
    font.base_add = 40;
    font.top_offset = 0;
    return font;
}

auto sample_request(std::string_view text) -> comicchat::MessagePanelRequest {
    comicchat::MessagePanelRequest request{};
    request.nick = "Ada";
    request.text = std::string{text};
    request.mode = comicchat::BalloonMode::say;
    request.font = say_font();
    request.body.avatar_id = 42;
    request.body.color = comicchat::nick_color("Ada");
    request.seed = comicchat::message_seed("Ada", text);
    return request;
}

} // namespace

TEST_CASE("build_message_panel assembles one body and one say balloon") {
    const auto request = sample_request("hello there world");
    const auto panel = comicchat::build_message_panel(request, monospace_measure(120));

    // Exactly one placed body, carrying the requested avatar identity/colour.
    REQUIRE(panel.bodies.size() == 1);
    CHECK(panel.bodies.front().avatar_id == 42);
    CHECK(panel.bodies.front().color == comicchat::nick_color("Ada"));
    // The body box has positive extent and sits at the panel floor.
    const auto& box = panel.bodies.front().box;
    CHECK(box.right > box.left);
    CHECK(box.top == comicchat::message_body_top);
    CHECK(box.top - box.bottom == comicchat::message_body_height);

    // Exactly one say balloon, wrapped and tailed at the speaker.
    REQUIRE(panel.balloons.size() == 1);
    const auto& balloon = panel.balloons.front();
    CHECK(balloon.kind.mode == comicchat::BalloonMode::say);
    CHECK_FALSE(balloon.kind.dashed);
    CHECK_FALSE(balloon.lines.empty());
    CHECK(balloon.has_tail);
    CHECK_FALSE(balloon.outline.empty());
    // The tail bottom anchors at the placed body's tail column (arrowX).
    CHECK(balloon.tail.anchor.x == panel.bodies.front().arrow_x);
    // The seed threads through unchanged for deterministic re-layout.
    CHECK(panel.seed == request.seed);
}

TEST_CASE("build_message_panel wraps text to the balloon width") {
    // 120-twip glyphs, 1200-twip max width -> ~10 glyphs/line incl. spaces. A
    // long single word is force-broken; the produced lines echo break_into_lines.
    const auto request = sample_request("alpha beta gamma delta epsilon zeta");
    const auto panel = comicchat::build_message_panel(request, monospace_measure(120));

    REQUIRE(panel.balloons.size() == 1);
    const auto& lines = panel.balloons.front().lines;
    const auto expected = comicchat::break_into_lines(monospace_measure(120),
                                                      comicchat::message_balloon_max_width,
                                                      request.text);
    REQUIRE(lines.size() == expected.size());
    CHECK(lines.size() > 1);  // it genuinely wrapped
    for (std::size_t i = 0; i < lines.size(); ++i) {
        CHECK(lines[i].text == expected[i].text);
        CHECK(lines[i].width <= comicchat::message_balloon_max_width);
    }
}

TEST_CASE("build_message_panel centres the cloud over the speaker anchor and stays in-panel") {
    const auto request = sample_request("centered");
    const auto panel = comicchat::build_message_panel(request, monospace_measure(120));

    REQUIRE(panel.balloons.size() == 1);
    const auto& box = panel.balloons.front().bbox;
    const std::int32_t center = (box.left + box.right) / 2;
    const std::int32_t arrow_x = panel.bodies.front().arrow_x;
    // Centred within one glyph of the anchor (integer rounding of width/2).
    CHECK(std::abs(center - arrow_x) <= 120);
    // The cloud stays inside the panel interior [0, logical_panel_width].
    CHECK(box.left >= 0);
    CHECK(box.right <= comicchat::logical_panel_width);
}

TEST_CASE("build_message_panel is deterministic") {
    const auto request = sample_request("same input same panel");
    const auto a = comicchat::build_message_panel(request, monospace_measure(120));
    const auto b = comicchat::build_message_panel(request, monospace_measure(120));

    REQUIRE(a.bodies.size() == b.bodies.size());
    REQUIRE(a.balloons.size() == b.balloons.size());
    CHECK(a.seed == b.seed);
    CHECK(a.bodies.front().box == b.bodies.front().box);
    CHECK(a.balloons.front().bbox == b.balloons.front().bbox);
    CHECK(a.balloons.front().outline == b.balloons.front().outline);
    CHECK(a.balloons.front().tail == b.balloons.front().tail);
}

TEST_CASE("empty message yields a body-only panel") {
    const auto request = sample_request("");
    const auto panel = comicchat::build_message_panel(request, monospace_measure(120));
    CHECK(panel.bodies.size() == 1);
    CHECK(panel.balloons.empty());
}

TEST_CASE("nick_color and message_seed are stable and nick-distinct") {
    CHECK(comicchat::nick_color("Ada") == comicchat::nick_color("Ada"));
    CHECK(comicchat::nick_color("Linus") == comicchat::nick_color("Linus"));
    CHECK(comicchat::message_seed("Ada", "hi") == comicchat::message_seed("Ada", "hi"));
    CHECK(comicchat::message_seed("Ada", "hi") != comicchat::message_seed("Ada", "bye"));
    CHECK(comicchat::message_seed("Ada", "hi") != comicchat::message_seed("Bob", "hi"));
}

TEST_CASE("build_say_panel builds a say panel from a live font engine") {
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto engine = comicchat::TextEngine::create(*font);
    REQUIRE(engine.has_value());

    const auto panel = comicchat::build_say_panel(**engine, "Ada", "Hello from IRC!");
    REQUIRE(panel.bodies.size() == 1);
    REQUIRE(panel.balloons.size() == 1);
    CHECK(panel.balloons.front().kind.mode == comicchat::BalloonMode::say);
    CHECK_FALSE(panel.balloons.front().lines.empty());
    CHECK(panel.bodies.front().color == comicchat::nick_color("Ada"));
}
