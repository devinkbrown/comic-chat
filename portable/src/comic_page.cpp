#include "comicchat/comic_page.hpp"

#include "comicchat/avatar_assets.hpp"
#include "comicchat/expression.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

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

// True when `dir` is a readable directory holding at least one `.avb` asset.
[[nodiscard]] auto has_avatar(const std::filesystem::path& dir) noexcept -> bool {
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) return false;
    std::filesystem::directory_iterator iterator{dir, error};
    if (error) return false;
    for (const auto& entry : iterator) {
        std::error_code file_error;
        if (entry.is_regular_file(file_error) && entry.path().extension() == ".avb") return true;
    }
    return false;
}

// --- CBWoodringNormal say-balloon kern derivation (fonts.cpp:82-83,89) --------
// The say balloon is MS CBWoodringNormal, built from m_fiWNormal whose CFontInfo
// vertical kern offsets are (int)(-40*reduction*doVKern), (int)(30*reduction*
// doVKern) with reduction = abs(lfHeight)/180.0 and doVKern = 1 IFF the resolved
// balloon face family is "Comic Sans MS" (fonts.cpp:82-83). These offsets map to
// build_font_metrics' (n_leading, n_base_add) arguments respectively.
inline constexpr double woodring_kern_reference_height = 180.0;  // fonts.cpp reduction denom
inline constexpr double woodring_leading_kern = -40.0;           // n_leading base (fonts.cpp:89)
inline constexpr double woodring_base_add_kern = 30.0;           // n_base_add base (fonts.cpp:89)

// doVKern (fonts.cpp:82-83): the CBWoodringNormal vertical kern is applied only
// when the balloon face actually resolves to Comic Sans MS. The portable build
// now ships real Comic Sans MS (commit "resolve bundled Comic Sans MS"), so this
// reads the engine's resolved FreeType family rather than assuming — a substitute
// face (e.g. the Comic Neue / DejaVu fallback) correctly yields doVKern = 0.
[[nodiscard]] auto face_is_comic_sans(const TextEngine& engine) noexcept -> bool {
    const auto* face = static_cast<FT_Face>(engine.native_face());
    if (face == nullptr || face->family_name == nullptr) return false;
    const std::string_view family{face->family_name};
    return family.find("Comic Sans") != std::string_view::npos;
}

// Resolve + load the nick's deterministically assigned avatar asset, or nullopt
// when no avatar directory/asset resolves. Shared by the PageAvatar dim wiring
// (nick_page_avatar) and the compositing provider (make_nick_avatar_provider) so
// both key off the identical nick->file assignment. Never throws.
[[nodiscard]] auto load_nick_avatar_asset(const std::string_view nick) -> std::optional<AvatarAsset> {
    const auto directory = find_avatar_directory();
    if (!directory) return std::nullopt;
    const auto names = available_avatars(*directory);
    const auto chosen = assign_avatar(nick, names);
    if (!chosen) return std::nullopt;
    auto asset = load_avatar_asset(*directory / std::string{*chosen});
    if (!asset.has_value()) return std::nullopt;
    return std::move(*asset);
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

auto nick_avatar_id(const std::string_view nick) noexcept -> std::uint32_t {
    return fnv1a(nick);
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

    auto balloon = layout_balloon(balloon_request);
    // Thread the measured font pixel size onto the balloon so render_panel draws
    // the text at the size the cloud was fitted to (drawn size == measured size).
    balloon.text_size = request.text_size;
    panel.balloons.push_back(std::move(balloon));
    return panel;
}

auto build_say_panel(TextEngine& engine, const std::string_view nick, const std::string_view text) -> Panel {
    // CBWoodringNormal say-balloon kern offsets from m_fiWNormal (fonts.cpp:89):
    // (int)(-40*reduction*doVKern), (int)(30*reduction*doVKern) with
    // reduction = message_text_size/180 and doVKern derived from the resolved
    // balloon face. At 240 twips with Comic Sans MS this is (-53, +40) — the
    // (int) casts truncate toward zero, matching MS. (These are NOT the Shout
    // font's (0, 0); with a non-zero raw n_leading the say balloon's m_topOffset
    // correctly keys to 0 rather than FAREAST_TOPOFFSET, balloon.cpp:635-638.)
    const double reduction = message_text_size / woodring_kern_reference_height;
    const int do_vkern = face_is_comic_sans(engine) ? 1 : 0;
    const auto n_leading =
        static_cast<std::int32_t>(woodring_leading_kern * reduction * do_vkern);
    const auto n_base_add =
        static_cast<std::int32_t>(woodring_base_add_kern * reduction * do_vkern);
    const auto metrics = build_font_metrics(engine, message_text_size, n_leading, n_base_add);

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

auto find_avatar_directory() -> std::optional<std::filesystem::path> {
    try {
        if (const char* override_path = std::getenv("COMICCHAT_AVATAR_DIR");
            override_path != nullptr && *override_path != '\0') {
            const std::filesystem::path candidate{override_path};
            if (has_avatar(candidate)) return candidate;
        }
#ifdef COMICCHAT_INSTALL_AVATAR_DIR
        if (const std::filesystem::path candidate{COMICCHAT_INSTALL_AVATAR_DIR};
            has_avatar(candidate)) return candidate;
#endif
#ifdef COMICCHAT_SOURCE_AVATAR_DIR
        if (const std::filesystem::path candidate{COMICCHAT_SOURCE_AVATAR_DIR};
            has_avatar(candidate)) return candidate;
#endif
        return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

auto available_avatars(const std::filesystem::path& dir) -> std::vector<std::string> {
    std::vector<std::string> names;
    std::error_code error;
    std::filesystem::directory_iterator iterator{dir, error};
    if (error) return names;
    for (const auto& entry : iterator) {
        std::error_code file_error;
        if (entry.is_regular_file(file_error) && entry.path().extension() == ".avb") {
            names.push_back(entry.path().filename().string());
        }
    }
    std::ranges::sort(names);
    return names;
}

auto assign_avatar(const std::string_view nick, const std::span<const std::string> names)
    -> std::optional<std::string_view> {
    if (names.empty()) return std::nullopt;
    return std::string_view{names[fnv1a(nick) % names.size()]};
}

auto nick_page_avatar(const std::string_view nick, const std::string_view text) -> PageAvatar {
    PageAvatar avatar;
    avatar.avatar_id = nick_avatar_id(nick);
    avatar.color = nick_color(nick);

    // Preferred path: the REAL per-avatar metric (avatar_dim_info == the
    // CBody::GetDimInfo port, panel.cpp:761). Feeds LayoutAvatars the true art
    // dims + head:body ratio so the zoom cap keeps the head on panel.
    if (auto asset = load_nick_avatar_asset(nick); asset.has_value()) {
        // Text -> emotion -> pose (expression.hpp/GetEmotionsFromString port):
        // an empty `text` infers no rules and select_avatar_expression's
        // SetBodyNeutral/SetFaceNeutral+SetTorsoNeutral fallback reproduces the
        // former hardcoded neutral exactly, so a caller that has no text yet
        // keeps today's behaviour unchanged.
        const auto emotions = emotions_from_text(text);
        const auto expression = select_avatar_expression(*asset, emotions);
        if (expression.has_value()) {
            // flip == false: face_fraction is the UNFLIPPED (right-pose) column;
            // layout.cpp mirrors it to (1 - face_fraction) when a body flips.
            const auto dim = avatar_dim_info(*asset, *expression, /*flip=*/false);
            if (dim.has_value() && dim->width > 0 && dim->height > 0) {
                avatar.body_width = dim->width;
                avatar.body_height = dim->height;
                avatar.norm_height = dim->height;  // standing height (GetDimInfo)
                // head_height is the true head-pixel span; guard a degenerate
                // metric with MS's simple-avatar value (avatar.cpp:63).
                avatar.head_height =
                    dim->head_height > 0 ? dim->head_height : dim->height / 2;
                avatar.face_fraction =
                    static_cast<double>(dim->face_x) / static_cast<double>(dim->width);
                return avatar;
            }
        }
    }

    // Fallback (no avatar asset resolves, or a degenerate metric): keep the
    // default body constants but set head_height to body_height/2 — MS's own
    // simple-avatar head value (avatar.cpp:63), NOT a guessed sub-half fraction —
    // so the "don't cut at neck" zoom cap (panel.cpp:797) stays conservative and
    // a lone speaker's head stays on panel.
    avatar.norm_height = avatar.body_height;      // normalize to own standing height
    avatar.head_height = avatar.body_height / 2;  // avatar.cpp:63
    avatar.face_fraction = 0.5;
    return avatar;
}

auto make_nick_avatar_provider(const std::string_view nick, const std::string_view text) -> PanelAvatarProvider {
    auto asset = load_nick_avatar_asset(nick);
    if (!asset.has_value()) return {};

    // Load the asset once; composite the text-derived pose per body render at
    // the exact device size render_panel asks for. Shared so the returned
    // provider stays copyable (PanelAvatarProvider is std::function).
    //
    // Own a copy of `text` (the caller's string_view is not guaranteed to
    // outlive the returned std::function) so every render call can re-derive
    // the same emotion -> pose selection. This is a pure function of
    // (asset, text): the same nick/text pair always resolves the same
    // AvatarSelection, so no per-nick "previous expression" state is needed
    // for correctness here — select_avatar_expression's `previous` parameter
    // only rotates among multiple same-emotion pose variants for visual
    // variety across repeated calls (see select_rotating_component), which is
    // orthogonal to picking the RIGHT expression for this line's text.
    auto shared_asset = std::make_shared<AvatarAsset>(std::move(*asset));
    auto owned_text = std::make_shared<std::string>(text);
    return [shared_asset, owned_text](const PanelBody& body, std::int32_t target_width,
                          std::int32_t target_height) -> std::optional<AvatarBitmap> {
        if (target_width <= 0 || target_height <= 0) return std::nullopt;
        const auto emotions = emotions_from_text(*owned_text);
        const auto expression = select_avatar_expression(*shared_asset, emotions);
        if (!expression.has_value()) return std::nullopt;
        auto raster = render_avatar(*shared_asset, {*expression, target_width, target_height, body.flip, false});
        if (!raster.has_value()) return std::nullopt;
        return std::move(*raster);
    };
}

} // namespace comicchat
