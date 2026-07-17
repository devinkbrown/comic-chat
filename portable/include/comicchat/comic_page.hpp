#pragma once

#include "comicchat/balloon.hpp"
#include "comicchat/cpp26.hpp"
#include "comicchat/layout.hpp"
#include "comicchat/text.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// Phase 2.5a — the first live feed increment (ROADMAP Phase 2.5 "app wiring").
//
// This is the smallest honest step that turns one incoming IRC channel line into
// a drawable comic Panel: it wires the completed renderer (layout_balloon,
// arrow_anchors, break_into_lines, build_font_metrics) behind a single
// message -> Panel builder that render_panel can draw.
//
// SCOPE (deliberately bounded — see build_message_panel's doc-comment):
//   * ONE speaker, ONE panel, ONE `say` balloon. Multi-speaker ordering
//     (order_conversation), multi-line page accounting, panel splitting
//     (should_start_new_panel), emotion/pose selection, action/whisper/think
//     mode routing, and real avatar-pixel compositing are the follow-up
//     (Phase 2.5b). The body is placed via the placement projection API
//     (arrow_anchors) and drawn as render_panel's colour-box placeholder; the
//     avatar bitmap is NOT yet blitted (the Canvas has no avatar path — that is
//     an explicit Phase 2.5b renderer task, not wiring).

namespace comicchat {

// ------------------------------------------------------------------------
// Panel-local geometry defaults (twips, Y-up) for the single-speaker slot.
// A Phase 2.5b avb-backed body would derive width/height/face_fraction from
// avatar_dim_info; these model a centred, right-facing speaker at the panel
// floor, matching the demo_panel proportions proven in render_test.cpp.
// ------------------------------------------------------------------------
inline constexpr std::int32_t message_body_width = 800;    // fitted body width
inline constexpr std::int32_t message_body_height = 840;   // fitted body height
inline constexpr std::int32_t message_body_top = -1400;    // body box top edge
inline constexpr std::int32_t message_balloon_max_width = 1200;  // wrap width
inline constexpr std::int32_t message_edge_margin = balloon_xborder;  // keep-in

// The say-balloon text size, in the same logical (twip) units the rest of the
// panel geometry uses. build_font_metrics/measure_text_width are evaluated at
// this size so the wrapped line widths and line_height are self-consistent with
// message_balloon_max_width above.
inline constexpr double message_text_size = 220.0;

// ------------------------------------------------------------------------
// One placed speaker body. The defaults produce a centred, right-facing slot;
// a caller with real avatar dimensions overrides width/height/face_fraction.
// ------------------------------------------------------------------------
struct SpeakerBody final {
    std::uint32_t avatar_id{};
    std::int32_t width{message_body_width};
    std::int32_t height{message_body_height};
    double face_fraction{0.5};  // faceX / width at the unflipped (right) pose
    bool flip{};                // facing: false = right, true = left
    std::uint32_t color{0x6c8ebfU};
};

// A single chat line to be assembled into a comic Panel.
struct MessagePanelRequest final {
    std::string nick;
    std::string text;
    BalloonMode mode{BalloonMode::say};
    SpeakerBody body{};
    FontMetrics font{};
    std::int32_t max_text_width{message_balloon_max_width};
    std::uint32_t seed{};
};

// Deterministic per-nick body tint, folded from an FNV-1a hash into a small
// curated comic palette so the same nick always draws the same colour.
[[nodiscard]] auto nick_color(std::string_view nick) noexcept -> std::uint32_t;

// Deterministic per-message panel seed (the CPanel::m_seed analogue). Stored on
// the Panel; a Phase 2.5b RNG-driven cloud_estimate placement would consume it.
[[nodiscard]] auto message_seed(std::string_view nick, std::string_view text) noexcept -> std::uint32_t;

// Assemble a single-speaker, single-panel comic Panel from one chat line.
//
// Places the speaker body with the placement projection (arrow_anchors),
// wraps the text with `measure` (break_into_lines), lays out the balloon with
// layout_balloon, and centres the cloud over the speaker's tail anchor. Pure
// and deterministic: inject a synthetic (e.g. monospace) `measure` to unit-test
// without a TTF, or measure_text_width(engine, size) for the live path.
//
// HONEST LIMIT: single speaker, single balloon, `say`/`whisper`/`think` tail
// modes only (action boxes are laid out but never split a panel here). The
// returned Panel always holds exactly one PanelBody; it holds one Balloon unless
// `text` wraps to zero lines (empty message), in which case balloons is empty.
[[nodiscard]] auto build_message_panel(const MessagePanelRequest& request, const TextMeasure& measure)
    -> Panel;

// Live-path convenience: derive the say-balloon FontMetrics and text measure
// from `engine` at message_text_size, colour the body from the nick, seed from
// (nick, text), and build a single say panel. Falls back to a zero FontMetrics
// only if the engine cannot be sized (never throws).
[[nodiscard]] auto build_say_panel(TextEngine& engine, std::string_view nick, std::string_view text)
    -> Panel;

} // namespace comicchat
