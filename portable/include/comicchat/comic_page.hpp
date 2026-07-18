#pragma once

#include "comicchat/balloon.hpp"
#include "comicchat/cpp26.hpp"
#include "comicchat/layout.hpp"
#include "comicchat/page.hpp"
#include "comicchat/render.hpp"
#include "comicchat/text.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
//
// Microsoft's default balloon font is PointsToTwips(12) = 12 * 20 = 240 twips
// (chat.cpp:387, chat.rc IDS_DFLT_COMICSPNTSIZE "12"; the pageview.cpp:174-183
// DPI terms cancel to points*20). This is a FIXED twip size: unlike the Title /
// Shout fonts it is NOT scaled by the m_unitWidth/4860 reduction (fonts.cpp:102).
inline constexpr double message_text_size = 240.0;

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
    // The font pixel size (twips) the balloon text is measured/drawn at. Stored on
    // the produced Balloon so render_panel draws the glyphs at exactly the size the
    // cloud was fitted to (message_text_size by default; keep it consistent with
    // the FontMetrics/TextMeasure the caller derives).
    double text_size{message_text_size};
};

// Deterministic per-nick body tint, folded from an FNV-1a hash into a small
// curated comic palette so the same nick always draws the same colour.
[[nodiscard]] auto nick_color(std::string_view nick) noexcept -> std::uint32_t;

// Deterministic per-nick avatar identity (the CAvatarX id analogue): the FNV-1a
// of the nick, the same fold nick_color and assign_avatar use. The live page
// wiring registers a PageAvatar under this id and keys its avatar provider by it,
// so a body's avatar_id resolves back to the speaker's composited raster.
[[nodiscard]] auto nick_avatar_id(std::string_view nick) noexcept -> std::uint32_t;

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

// ------------------------------------------------------------------------
// Phase 2.5b — provisioned avatar set + deterministic nick->avatar mapping.
// ------------------------------------------------------------------------

// Resolve the runtime directory holding the default `.avb` avatar set. Probes,
// in order: the COMICCHAT_AVATAR_DIR environment override, the installed datadir
// avatar dir (-DCOMICCHAT_INSTALL_AVATAR_DIR), then the in-tree comicart source
// dir (-DCOMICCHAT_SOURCE_AVATAR_DIR) as a dev fallback. Returns nullopt when no
// candidate holds at least one `.avb`. Never throws.
[[nodiscard]] auto find_avatar_directory() -> std::optional<std::filesystem::path>;

// The available avatar file names (e.g. "anna.avb") in `dir`, sorted
// lexicographically so the nick->avatar assignment is independent of filesystem
// enumeration order. Empty on a missing/unreadable directory. Never throws.
[[nodiscard]] auto available_avatars(const std::filesystem::path& dir) -> std::vector<std::string>;

// Deterministic per-nick avatar choice: fold the nick with the same FNV-1a used
// by nick_color and index into the sorted `names`. Pure and testable without a
// filesystem. Returns nullopt only when `names` is empty.
[[nodiscard]] auto assign_avatar(std::string_view nick, std::span<const std::string> names)
    -> std::optional<std::string_view>;

// Build a PanelAvatarProvider that loads the nick's deterministically assigned
// avatar from the resolved runtime directory and composites the TEXT-DERIVED
// pose (expression.hpp's emotions_from_text over default_emotion_rules(), fed
// through the multi-emotion select_avatar_expression) at the exact device size
// render_panel requests (honoring PanelBody::flip). `text` is the chat line
// this avatar is reacting to; an empty/default `text` infers no emotion rules
// and select_avatar_expression's neutral fallback reproduces the plain neutral
// pose. The avatar asset is loaded once, here, and reused per body render; the
// pose is a pure function of (nick, text) so it is safely re-derived on every
// render call with no per-nick state. Returns an empty provider (nullptr) when
// no avatar directory/asset resolves so render_panel keeps the flat color-box
// fallback. Never throws.
[[nodiscard]] auto make_nick_avatar_provider(std::string_view nick, std::string_view text = {})
    -> PanelAvatarProvider;

// Resolve the nick's deterministically assigned avatar and populate a page.hpp
// PageAvatar with the REAL per-avatar dimensions from avatar_dim_info (the
// CBody::GetDimInfo port, panel.cpp:761): body_width / body_height (the art dims
// LayoutAvatars normalizes onto maxBodyHeight), norm_height (the standing height
// it normalizes by), head_height (the true head-pixel span the "don't cut at
// neck" zoom cap consults, panel.cpp:797), and face_fraction (the tail column).
// Feeding the real head:body ratio is what keeps a lone speaker's head on panel
// instead of the over-zoom a guessed head fraction permits.
//
// This is the avatar-selection path that resolves the nick's AvatarAsset (the
// same deterministic assignment make_nick_avatar_provider uses), so the body it
// dimensions and the raster the provider composites are the SAME avatar. The
// live page wiring feeds this PageAvatar straight into Page::add_line.
//
// `text` is the chat line this avatar is reacting to: it is run through
// expression.hpp's emotions_from_text(default_emotion_rules()) and the result
// resolves the pose via the multi-emotion select_avatar_expression, so a body
// dimensioned for e.g. a gesture/shout pose reflects that pose's real
// head:body ratio rather than always the neutral one. An empty/default `text`
// infers no emotion rules, and select_avatar_expression's neutral fallback
// reproduces the prior hardcoded-neutral behaviour exactly.
//
// When no avatar asset resolves (or its metric is degenerate), returns a
// PageAvatar carrying the default body constants with head_height set to
// body_height/2 — Microsoft's own simple-avatar head value (avatar.cpp:63) — so
// the zoom cap stays conservative. Never a guessed sub-half fraction; never
// throws.
[[nodiscard]] auto nick_page_avatar(std::string_view nick, std::string_view text = {}) -> PageAvatar;

} // namespace comicchat
