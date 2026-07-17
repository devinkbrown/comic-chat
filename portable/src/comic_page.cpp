#include "comicchat/comic_page.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace comicchat {
namespace {

// FNV-1a over UTF-8 bytes. Small, stable, endian-independent — used for both the
// per-nick colour index and the per-message seed so the same input always maps
// to the same panel.
[[nodiscard]] auto fnv1a(std::string_view text) noexcept -> std::uint32_t {
    std::uint32_t hash = 2166136261U;
    for (const char byte : text) {
        hash ^= static_cast<std::uint8_t>(byte);
        hash *= 16777619U;
    }
    return hash;
}

// The desired left edge of the balloon cloud so it centres over the speaker's
// tail anchor while staying inside the panel interior.
[[nodiscard]] auto centered_left(std::int32_t arrow_x, std::int32_t width) noexcept -> std::int32_t {
    const std::int32_t max_left = logical_panel_width - message_edge_margin - width;
    std::int32_t left = arrow_x - width / 2;
    left = std::max(left, message_edge_margin);
    if (max_left >= message_edge_margin) left = std::min(left, max_left);
    return left;
}

} // namespace

auto nick_color(const std::string_view nick) noexcept -> std::uint32_t {
    // A curated comic palette (the title-card stars plus complementary hues):
    // saturated but legible against a white panel with black balloon ink.
    static constexpr std::array<std::uint32_t, 8> palette{
        0x456990U, 0xb85c5cU, 0x6b986bU, 0xc08a3eU,
        0x7d6bb0U, 0x3f9aa0U, 0xb05c8eU, 0x8a8f3eU,
    };
    return palette[fnv1a(nick) % palette.size()];
}

auto message_seed(const std::string_view nick, const std::string_view text) noexcept -> std::uint32_t {
    // Fold nick and text together so distinct speakers/lines seed distinctly but
    // an identical line reproduces the same panel byte-for-byte.
    return fnv1a(nick) * 16777619U ^ fnv1a(text);
}

auto build_message_panel(const MessagePanelRequest& request, const TextMeasure& measure) -> Panel {
    Panel panel;
    panel.seed = request.seed;

    // --- Body placement (Item 2.3 projection API). One slot, centred. --------
    const std::int32_t interior_left = std::max(message_edge_margin,
                                                (logical_panel_width - request.body.width) / 2);
    const std::vector<BodySlot> slots{
        BodySlot{request.body.avatar_id, request.body.width, request.body.face_fraction, request.body.flip},
    };
    const auto anchors = arrow_anchors(slots, interior_left, 0);
    const auto& anchor = anchors.front();

    PanelBody body{};
    body.avatar_id = request.body.avatar_id;
    body.box = Rect{anchor.left, message_body_top - request.body.height,
                    anchor.left + request.body.width, message_body_top};
    body.arrow_x = anchor.arrow_x;
    body.color = request.body.color;
    body.flip = request.body.flip;
    panel.bodies.push_back(body);

    // --- Text wrap (Item 2.4). Empty message -> body-only panel. -------------
    const auto lines = break_into_lines(measure, request.max_text_width, request.text);
    const bool has_text = std::any_of(lines.begin(), lines.end(),
                                      [](const TextLine& line) { return !line.text.empty(); });
    if (!has_text) return panel;

    // --- Balloon layout (Item 2.1). Two passes: the first measures the cloud
    //     width at place_left = 0, the second centres it over the tail anchor.
    //     bbox.left is linear in place_left with slope 1, so the width is
    //     invariant and the recentring is exact.
    BalloonRequest balloon_request{};
    balloon_request.kind = select_balloon_mode(
        request.mode == BalloonMode::whisper ? bm_whisper
        : request.mode == BalloonMode::think ? bm_think
        : request.mode == BalloonMode::action ? bm_action
                                              : bm_say);
    balloon_request.text = request.text;
    balloon_request.lines = lines;
    balloon_request.font = request.font;
    balloon_request.arrow_x = anchor.arrow_x;
    balloon_request.speaker_top = message_body_top;
    balloon_request.place_left = 0;
    balloon_request.place_top = 0;

    const auto probe = layout_balloon(balloon_request);
    const std::int32_t width = probe.bbox.right - probe.bbox.left;
    const std::int32_t desired_left = centered_left(anchor.arrow_x, width);
    balloon_request.place_left = desired_left - probe.bbox.left;

    panel.balloons.push_back(layout_balloon(balloon_request));
    return panel;
}

auto build_say_panel(TextEngine& engine, const std::string_view nick, const std::string_view text) -> Panel {
    // Shout-font kern offsets (0, 0), matching fonts.cpp's balloon face; the
    // raw n_leading = 0 keys the far-east top offset exactly as the source.
    const auto metrics = build_font_metrics(engine, message_text_size, 0, 0);

    MessagePanelRequest request{};
    request.nick = std::string{nick};
    request.text = std::string{text};
    request.mode = BalloonMode::say;
    request.font = metrics.value_or(FontMetrics{});
    request.body.avatar_id = fnv1a(nick);
    request.body.color = nick_color(nick);
    request.seed = message_seed(nick, text);

    const auto measure = measure_text_width(engine, message_text_size);
    return build_message_panel(request, measure);
}

} // namespace comicchat
