#include "comicchat/avatar_assets.hpp"
#include "comicchat/balloon.hpp"
#include "comicchat/comic_page.hpp"
#include "comicchat/layout.hpp"
#include "comicchat/page.hpp"
#include "comicchat/text.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

TEST_CASE("build_say_panel applies the CBWoodringNormal kern (doVKern=1), not the shout (0,0)") {
    // The bundled comic.ttf resolves to the "Comic Sans MS" family, so the
    // say balloon's doVKern is 1 (fonts.cpp:82-83) and m_fiWNormal carries the
    // Woodring vertical kern (fonts.cpp:89): (int)(-40*r), (int)(30*r) with
    // r = message_text_size/180. At 240 twips that truncates to (-53, +40).
    const auto font = comicchat::find_portable_comic_font();
    REQUIRE(font.has_value());
    auto engine = comicchat::TextEngine::create(*font);
    REQUIRE(engine.has_value());

    const double reduction = comicchat::message_text_size / 180.0;
    const auto n_leading = static_cast<std::int32_t>(-40.0 * reduction);
    const auto n_base_add = static_cast<std::int32_t>(30.0 * reduction);
    REQUIRE(n_leading == -53);  // (int) truncation toward zero, as MS
    REQUIRE(n_base_add == 40);

    const auto kerned = comicchat::build_font_metrics(**engine, comicchat::message_text_size,
                                                      n_leading, n_base_add);
    const auto shout0 = comicchat::build_font_metrics(**engine, comicchat::message_text_size, 0, 0);
    REQUIRE(kerned.has_value());
    REQUIRE(shout0.has_value());
    // A non-zero raw n_leading suppresses the far-east top offset (balloon.cpp:635-638):
    // the say balloon keys top_offset to 0, unlike the retired (0,0) which kept 50.
    CHECK(kerned->top_offset == 0);
    CHECK(shout0->top_offset == comicchat::far_east_top_offset);
    CHECK(*kerned != *shout0);

    // Prove build_say_panel actually laid the balloon with the kerned metrics:
    // the same request built explicitly from *kerned reproduces its bbox, and the
    // (0,0) shout metrics would NOT (different line_height / base_add).
    const auto say = comicchat::build_say_panel(**engine, "Ada", "hi");
    REQUIRE(say.balloons.size() == 1);

    comicchat::MessagePanelRequest request{};
    request.nick = "Ada";
    request.text = "hi";
    request.mode = comicchat::BalloonMode::say;
    request.font = *kerned;
    request.body.avatar_id = comicchat::nick_avatar_id("Ada");
    request.body.color = comicchat::nick_color("Ada");
    request.seed = comicchat::message_seed("Ada", "hi");
    const auto measure = comicchat::measure_text_width(**engine, comicchat::message_text_size);
    const auto expected = comicchat::build_message_panel(request, measure);
    REQUIRE(expected.balloons.size() == 1);
    CHECK(say.balloons.front().bbox == expected.balloons.front().bbox);

    request.font = *shout0;
    const auto shout_panel = comicchat::build_message_panel(request, measure);
    REQUIRE(shout_panel.balloons.size() == 1);
    CHECK(say.balloons.front().bbox != shout_panel.balloons.front().bbox);
}

// ---------------------------------------------------------------------------
// Phase 2.5b — nick->avatar provisioning + assignment.
// ---------------------------------------------------------------------------

TEST_CASE("assign_avatar is deterministic, stable, and in-range") {
    const std::vector<std::string> roster{
        "anna.avb", "buck.avb", "cro.avb", "dan.avb", "xeno.avb",
    };
    // Same nick always maps to the same file across repeated calls.
    const auto first = comicchat::assign_avatar("Ada", roster);
    const auto again = comicchat::assign_avatar("Ada", roster);
    REQUIRE(first.has_value());
    REQUIRE(again.has_value());
    CHECK(*first == *again);
    // The choice is always a member of the roster.
    CHECK(std::find(roster.begin(), roster.end(), std::string{*first}) != roster.end());
    // Distinct nicks can (and here do) land on distinct avatars.
    const auto linus = comicchat::assign_avatar("Linus", roster);
    REQUIRE(linus.has_value());
    CHECK(std::find(roster.begin(), roster.end(), std::string{*linus}) != roster.end());
}

TEST_CASE("assign_avatar returns nullopt for an empty roster") {
    const std::vector<std::string> empty{};
    CHECK_FALSE(comicchat::assign_avatar("Ada", empty).has_value());
}

TEST_CASE("available_avatars lists the comicart .avb set sorted") {
    const auto names = comicchat::available_avatars(COMICCHAT_TEST_COMICART_DIR);
    REQUIRE_FALSE(names.empty());
    // Sorted, so enumeration order does not perturb the assignment.
    CHECK(std::is_sorted(names.begin(), names.end()));
    // Every entry is an .avb (the .bgb backdrops are excluded).
    for (const auto& name : names) {
        CHECK(std::filesystem::path{name}.extension() == ".avb");
    }
    // The shipped corpus includes the known avatars used elsewhere in the suite.
    CHECK(std::find(names.begin(), names.end(), "xeno.avb") != names.end());
}

TEST_CASE("find_avatar_directory honors the COMICCHAT_AVATAR_DIR override") {
    setenv("COMICCHAT_AVATAR_DIR", COMICCHAT_TEST_COMICART_DIR, 1);
    const auto directory = comicchat::find_avatar_directory();
    unsetenv("COMICCHAT_AVATAR_DIR");
    REQUIRE(directory.has_value());
    CHECK(std::filesystem::equivalent(*directory,
                                      std::filesystem::path{COMICCHAT_TEST_COMICART_DIR}));
}

TEST_CASE("make_nick_avatar_provider composites a loadable avatar for a nick") {
    setenv("COMICCHAT_AVATAR_DIR", COMICCHAT_TEST_COMICART_DIR, 1);
    const auto provider = comicchat::make_nick_avatar_provider("Ada");
    unsetenv("COMICCHAT_AVATAR_DIR");
    REQUIRE(static_cast<bool>(provider));

    comicchat::PanelBody body{};
    body.avatar_id = 1;
    body.flip = false;
    const auto raster = provider(body, 120, 160);
    REQUIRE(raster.has_value());
    CHECK(raster->width == 120);
    CHECK(raster->height == 160);
    CHECK(raster->pixels.size() == static_cast<std::size_t>(120) * 160);

    // Deterministic: same body + size -> byte-identical raster.
    const auto again = provider(body, 120, 160);
    REQUIRE(again.has_value());
    CHECK(raster->pixels == again->pixels);

    // A non-positive target keeps the color-box fallback (nullopt).
    CHECK_FALSE(provider(body, 0, 160).has_value());
}

TEST_CASE("make_nick_avatar_provider is empty when no avatar set resolves") {
    setenv("COMICCHAT_AVATAR_DIR", "/nonexistent/comicchat/avatars", 1);
    // Guard against a stray install/source dir satisfying the fallback: only
    // assert the empty-provider contract when nothing else resolves either.
    const auto directory = comicchat::find_avatar_directory();
    const auto provider = comicchat::make_nick_avatar_provider("Ada");
    unsetenv("COMICCHAT_AVATAR_DIR");
    if (!directory.has_value()) {
        CHECK_FALSE(static_cast<bool>(provider));
    } else {
        SUCCEED("an install/source avatar dir is present; fallback resolved");
    }
}

// ---------------------------------------------------------------------------
// FIX 3 — real per-avatar dims from avatar_dim_info (kills the head over-zoom).
// ---------------------------------------------------------------------------

TEST_CASE("nick_page_avatar populates head_height from avatar_dim_info, not a fraction") {
    setenv("COMICCHAT_AVATAR_DIR", COMICCHAT_TEST_COMICART_DIR, 1);
    const auto avatar = comicchat::nick_page_avatar("Ada");

    // Recompute the expected metric through the SAME deterministic assignment so
    // the assertion is the real GetDimInfo port, not a hand-picked number.
    const auto directory = comicchat::find_avatar_directory();
    REQUIRE(directory.has_value());
    const auto names = comicchat::available_avatars(*directory);
    const auto chosen = comicchat::assign_avatar("Ada", names);
    REQUIRE(chosen.has_value());
    const auto asset = comicchat::load_avatar_asset(*directory / std::string{*chosen});
    REQUIRE(asset.has_value());
    const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
    REQUIRE(neutral.has_value());
    const auto dim = comicchat::avatar_dim_info(*asset, *neutral, false);
    REQUIRE(dim.has_value());
    unsetenv("COMICCHAT_AVATAR_DIR");

    CHECK(avatar.avatar_id == comicchat::nick_avatar_id("Ada"));
    CHECK(avatar.body_width == dim->width);
    CHECK(avatar.body_height == dim->height);
    CHECK(avatar.norm_height == dim->height);  // standing height
    CHECK(avatar.head_height == dim->head_height);
    // The head is a real span, NOT the retired guessed 0.32*body_height fraction.
    const auto guessed = static_cast<std::int32_t>(
        static_cast<double>(avatar.body_height) * 0.32 + 0.5);
    CHECK(avatar.head_height != guessed);
    CHECK(avatar.head_height > 0);
    const double expected_face_fraction =
        static_cast<double>(dim->face_x) / static_cast<double>(dim->width);
    CHECK(avatar.face_fraction == expected_face_fraction);
}

TEST_CASE("nick_page_avatar falls back to body_height/2 (not a guessed fraction) with no asset") {
    setenv("COMICCHAT_AVATAR_DIR", "/nonexistent/comicchat/avatars", 1);
    const auto resolvable = comicchat::find_avatar_directory().has_value();
    const auto avatar = comicchat::nick_page_avatar("Ada");
    unsetenv("COMICCHAT_AVATAR_DIR");

    if (resolvable) {
        SUCCEED("an install/source avatar dir is present; real metric resolved");
        return;
    }
    // MS's own simple-avatar head value is body_height/2 (avatar.cpp:63), NOT the
    // retired 0.32 fraction that let a lone speaker over-zoom off panel.
    CHECK(avatar.head_height == avatar.body_height / 2);
    CHECK(avatar.norm_height == avatar.body_height);
    CHECK(avatar.face_fraction == 0.5);
    const auto guessed = static_cast<std::int32_t>(
        static_cast<double>(avatar.body_height) * 0.32 + 0.5);
    CHECK(avatar.head_height != guessed);
}

namespace {

// Drive one lone-speaker line through the real page-composition path
// (LayoutAvatars zoom cap) and return the placed body's scaled height.
auto lone_speaker_body_height(const comicchat::PageAvatar& speaker) -> std::int32_t {
    comicchat::PageConfig config{};
    config.font = say_font();
    config.text_size = comicchat::message_text_size;
    comicchat::Page page{config, monospace_measure(120)};
    page.add_line(comicchat::Line{speaker, "hi", comicchat::bm_say});
    REQUIRE_FALSE(page.panels().empty());
    REQUIRE(page.panels().front().bodies.size() == 1);
    const auto& box = page.panels().front().bodies.front().box;
    return box.top - box.bottom;
}

} // namespace

TEST_CASE("body_height/2 head caps the zoom so a lone speaker fits; the 0.32 guess over-zooms") {
    // Same body dims for both, isolating the head-fraction effect on the
    // LayoutAvatars "don't cut at neck" zoom cap (panel.cpp:797).
    comicchat::PageAvatar fixed{};
    fixed.avatar_id = 1;
    fixed.body_width = comicchat::page_default_body_width;    // 800
    fixed.body_height = comicchat::page_default_body_height;  // 840
    fixed.norm_height = fixed.body_height;
    fixed.head_height = fixed.body_height / 2;  // avatar.cpp:63 / nick_page_avatar fallback

    // head_height == 0 exercises page.cpp's retired 0.32*body_height fallback.
    comicchat::PageAvatar guessed = fixed;
    guessed.avatar_id = 2;
    guessed.head_height = 0;

    const std::int32_t fixed_height = lone_speaker_body_height(fixed);
    const std::int32_t guessed_height = lone_speaker_body_height(guessed);

    // The realistic body_height/2 head fraction keeps the scaled body within the
    // panel (feet on the floor, head on panel), where the smaller guessed head
    // fraction let the zoom run past the panel height.
    CHECK(fixed_height <= comicchat::logical_panel_height);
    CHECK(guessed_height > fixed_height);
}

TEST_CASE("real avatar_dim_info dims keep a normally-proportioned lone speaker on panel") {
    // Load a known well-proportioned avatar directly (jordan.avb: a simple avatar
    // whose head is body_height/2, matching MS's CBodySingle value avatar.cpp:63)
    // and dimension a PageAvatar from the real GetDimInfo port, exactly as
    // nick_page_avatar does for whichever asset a nick resolves to.
    const std::filesystem::path avb{std::filesystem::path{COMICCHAT_TEST_COMICART_DIR} / "jordan.avb"};
    const auto asset = comicchat::load_avatar_asset(avb);
    REQUIRE(asset.has_value());
    const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
    REQUIRE(neutral.has_value());
    const auto dim = comicchat::avatar_dim_info(*asset, *neutral, false);
    REQUIRE(dim.has_value());

    comicchat::PageAvatar speaker{};
    speaker.avatar_id = 7;
    speaker.body_width = dim->width;
    speaker.body_height = dim->height;
    speaker.norm_height = dim->height;
    speaker.head_height = dim->head_height;
    speaker.face_fraction = static_cast<double>(dim->face_x) / static_cast<double>(dim->width);

    // Feet on the floor, head on panel: the true head:body ratio caps the zoom so
    // the scaled body fits the panel height.
    const std::int32_t height = lone_speaker_body_height(speaker);
    CHECK(speaker.head_height > 0);
    CHECK(height <= comicchat::logical_panel_height);
}
