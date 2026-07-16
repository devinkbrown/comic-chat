#pragma once

#include "comicchat/cpp26.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace comicchat {

enum class AvatarAssetError {
    io,
    truncated,
    invalid_header,
    invalid_record,
    invalid_offset,
    invalid_palette,
    invalid_bitmap,
    unsupported_format,
    decompression,
    allocation,
};

enum class AvatarKind : std::uint16_t { simple = 1, complex = 2, backdrop = 3 };

// Pixels use the same in-memory byte order as Cairo's RGB24/ARGB32 image
// surfaces on little-endian Windows and Linux: 0xAARRGGBB.
struct AvatarBitmap final {
    std::int32_t width{};
    std::int32_t height{};
    std::vector<std::uint32_t> pixels;
};

struct AvatarPose final {
    std::optional<AvatarBitmap> drawing;
    std::optional<AvatarBitmap> mask;
    std::optional<AvatarBitmap> aura;
};

struct AvatarComponent final {
    std::uint16_t pose_id{};
    std::uint16_t emotion_index{};
    std::uint8_t intensity{};
    std::int16_t center_x{};
    std::int16_t center_y{};
    std::int16_t center_delta_x{};
    std::int16_t center_delta_y{};
    std::int16_t face_x{};
    std::int16_t face_y{};
};

struct AvatarAsset final {
    AvatarKind kind{};
    std::string name;
    std::string copyright;
    std::string original_url;
    std::string override_url;
    std::uint8_t style{};
    std::uint8_t flags{};
    std::uint16_t icon_pose_id{};
    std::vector<AvatarPose> poses;
    std::vector<AvatarComponent> bodies;
    std::vector<AvatarComponent> faces;
    std::vector<AvatarComponent> torsos;
    std::optional<AvatarBitmap> backdrop;
};

struct AvatarExpression final {
    double angle{};
    double intensity{};
};

struct AvatarSelection final {
    std::size_t body{};
    std::size_t face{};
    std::size_t torso{};
};

struct AvatarRenderRequest final {
    AvatarSelection selection;
    std::int32_t width{};
    std::int32_t height{};
    bool flip{};
    bool draw_nimbus{};
};

[[nodiscard]] auto load_avatar_asset(const std::filesystem::path& path)
    -> std::expected<AvatarAsset, AvatarAssetError>;
[[nodiscard]] auto select_avatar_expression(const AvatarAsset& asset, AvatarExpression expression,
    std::optional<AvatarSelection> previous = std::nullopt)
    -> std::expected<AvatarSelection, AvatarAssetError>;
[[nodiscard]] auto render_avatar(const AvatarAsset& asset, const AvatarRenderRequest& request)
    -> std::expected<AvatarBitmap, AvatarAssetError>;
[[nodiscard]] auto write_avatar_png(const AvatarBitmap& bitmap, const std::filesystem::path& path) -> bool;

} // namespace comicchat
