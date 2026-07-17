#pragma once

#include "comicchat/cpp26.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace comicchat {

enum class SourceRasterError {
    invalid_stream,
    unsupported_format,
    io,
    allocation,
    dimension_mismatch,
    source_directory_missing,
};

struct RasterImage final {
    std::uint32_t width{};
    std::uint32_t height{};
    // Straight-alpha pixels in 0xAARRGGBB integer form. This is directly
    // compatible with SDL_PIXELFORMAT_ARGB8888 on every SDL-supported endian.
    std::vector<std::uint32_t> argb;
};

struct RasterCell final {
    std::uint16_t x{};
    std::uint16_t y{};
    std::uint16_t width{};
    std::uint16_t height{};
    std::string_view semantic;

    auto operator==(const RasterCell&) const -> bool = default;
};

enum class SourceIcon : std::uint8_t {
    application,
    document,
    room,
    ruleset,
    avatar,
    background,
    ratings,
    whisper,
    notification,
    connect_server,
    connect_network,
};

struct SourceIconSpec final {
    SourceIcon id{};
    std::string_view file_name;
    std::string_view semantic;
};

enum class SourceStrip : std::uint8_t {
    say_toolbar,
    about_tiki,
    main_toolbar,
    tabs,
    member_status,
    old_new,
    connection,
    rule_stopped,
    rule_inactive,
    rule_active,
    text_toolbar,
    user_toolbar,
};

struct SourceStripSpec final {
    SourceStrip id{};
    std::string_view file_name;
    std::uint16_t width{};
    std::uint16_t height{};
    // RGB color key from Microsoft's original consumer, when one exists.
    // A missing key means that every decoded pixel is intentionally opaque.
    std::optional<std::uint32_t> transparent_rgb;
    std::span<const RasterCell> cells;
};

[[nodiscard]] auto source_icon_catalog() noexcept -> std::span<const SourceIconSpec>;
[[nodiscard]] auto source_strip_catalog() noexcept -> std::span<const SourceStripSpec>;
[[nodiscard]] auto source_icon_spec(SourceIcon id) noexcept -> const SourceIconSpec&;
[[nodiscard]] auto source_strip_spec(SourceStrip id) noexcept -> const SourceStripSpec&;

[[nodiscard]] auto decode_windows_icon(std::span<const std::byte> bytes, std::uint32_t preferred_width)
    -> std::expected<RasterImage, SourceRasterError>;
[[nodiscard]] auto decode_windows_bitmap(std::span<const std::byte> bytes)
    -> std::expected<RasterImage, SourceRasterError>;

[[nodiscard]] auto load_source_icon(
    const std::filesystem::path& source_directory, SourceIcon id, std::uint32_t preferred_width)
    -> std::expected<RasterImage, SourceRasterError>;
[[nodiscard]] auto load_source_strip(const std::filesystem::path& source_directory, SourceStrip id)
    -> std::expected<RasterImage, SourceRasterError>;

// Resolve only the released Microsoft resource directory: an explicit
// COMICCHAT_SOURCE_RASTER_DIR override, the installed data directory, or a
// nearby source checkout. No generated/replacement art is considered.
[[nodiscard]] auto find_source_raster_directory()
    -> std::expected<std::filesystem::path, SourceRasterError>;

} // namespace comicchat
