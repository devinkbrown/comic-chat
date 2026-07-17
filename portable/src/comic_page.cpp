#include "comicchat/comic_page.hpp"

#include "comicchat/avatar_assets.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
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

auto make_nick_avatar_provider(const std::string_view nick) -> PanelAvatarProvider {
    const auto directory = find_avatar_directory();
    if (!directory) return {};
    const auto names = available_avatars(*directory);
    const auto chosen = assign_avatar(nick, names);
    if (!chosen) return {};

    auto asset = load_avatar_asset(*directory / std::string{*chosen});
    if (!asset.has_value()) return {};

    // Load the asset once; composite its neutral pose per body render at the
    // exact device size render_panel asks for. Shared so the returned provider
    // stays copyable (PanelAvatarProvider is std::function).
    auto shared_asset = std::make_shared<AvatarAsset>(std::move(*asset));
    return [shared_asset](const PanelBody& body, std::int32_t target_width,
                          std::int32_t target_height) -> std::optional<AvatarBitmap> {
        if (target_width <= 0 || target_height <= 0) return std::nullopt;
        const auto neutral = select_avatar_expression(*shared_asset, {0.0, 0.0});
        if (!neutral.has_value()) return std::nullopt;
        auto raster = render_avatar(*shared_asset, {*neutral, target_width, target_height, body.flip, false});
        if (!raster.has_value()) return std::nullopt;
        return std::move(*raster);
    };
}

} // namespace comicchat
