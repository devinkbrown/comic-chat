#include "comicchat/source_raster.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <string>

namespace comicchat {
namespace {

constexpr std::uint32_t rgb_silver = 0x00c0'c0c0U;
constexpr std::uint32_t rgb_blue = 0x0000'00ffU;
constexpr std::uint32_t rgb_green = 0x0000'ff00U;
constexpr std::size_t maximum_source_file_size = 16U * 1024U * 1024U;

constexpr std::array say_cells{
    RasterCell{0, 0, 17, 17, "say"},
    RasterCell{17, 0, 17, 17, "think"},
    RasterCell{34, 0, 17, 17, "whisper"},
    RasterCell{51, 0, 17, 17, "action"},
    RasterCell{68, 0, 17, 17, "whisper_action"},
    RasterCell{85, 0, 17, 17, "sound"},
    RasterCell{102, 0, 16, 17, "whisper_sound"},
};
constexpr std::array tiki_cells{RasterCell{0, 0, 490, 250, "about_background"}};
constexpr std::array main_toolbar_cells{
    RasterCell{0, 0, 16, 16, "connect"},
    RasterCell{16, 0, 16, 16, "disconnect"},
    RasterCell{32, 0, 16, 16, "enter_room"},
    RasterCell{48, 0, 16, 16, "leave_room"},
    RasterCell{64, 0, 16, 16, "create_room"},
    RasterCell{80, 0, 16, 16, "comic_view"},
    RasterCell{96, 0, 16, 16, "text_view"},
    RasterCell{112, 0, 16, 16, "room_list"},
    RasterCell{128, 0, 16, 16, "user_list"},
    RasterCell{144, 0, 16, 16, "favorites"},
};
constexpr std::array tab_cells{
    RasterCell{0, 0, 16, 16, "room"},
    RasterCell{16, 0, 16, 16, "new_room"},
    RasterCell{32, 0, 16, 16, "status"},
    RasterCell{48, 0, 16, 16, "alert"},
};
constexpr std::array member_cells{
    RasterCell{0, 0, 16, 16, "normal"},
    RasterCell{16, 0, 16, 16, "host"},
    RasterCell{32, 0, 16, 16, "spectator"},
    RasterCell{48, 0, 16, 16, "ignored"},
    RasterCell{64, 0, 16, 16, "away"},
};
constexpr std::array old_new_cells{RasterCell{0, 0, 16, 16, "new_indicator"}};
constexpr std::array connection_cells{
    RasterCell{0, 0, 16, 16, "connected"},
    RasterCell{16, 0, 16, 16, "disconnected"},
};
constexpr std::array stopped_cells{RasterCell{0, 0, 16, 15, "stopped"}};
constexpr std::array inactive_cells{RasterCell{0, 0, 16, 15, "inactive"}};
constexpr std::array active_cells{RasterCell{0, 0, 16, 15, "active"}};
constexpr std::array text_toolbar_cells{
    RasterCell{0, 0, 16, 16, "font"},
    RasterCell{16, 0, 16, 16, "color"},
    RasterCell{32, 0, 16, 16, "bold"},
    RasterCell{48, 0, 16, 16, "italic"},
    RasterCell{64, 0, 16, 16, "underline"},
    RasterCell{80, 0, 16, 16, "fixed_pitch"},
    RasterCell{96, 0, 16, 16, "symbol"},
};
constexpr std::array user_toolbar_cells{
    RasterCell{0, 0, 16, 16, "away"},
    RasterCell{16, 0, 16, 16, "identity"},
    RasterCell{32, 0, 16, 16, "ignore"},
    RasterCell{48, 0, 16, 16, "whisper"},
    RasterCell{64, 0, 16, 16, "email"},
    RasterCell{80, 0, 16, 16, "homepage"},
    RasterCell{96, 0, 16, 16, "netmeeting"},
};

constexpr std::array icon_catalog{
    SourceIconSpec{SourceIcon::application, "chat.ico", "application"},
    SourceIconSpec{SourceIcon::document, "chatdoc.ico", "chat_document"},
    SourceIconSpec{SourceIcon::room, "room.ico", "chat_room"},
    SourceIconSpec{SourceIcon::ruleset, "ruleset.ico", "ruleset_document"},
    SourceIconSpec{SourceIcon::avatar, "avatar.ico", "avatar_document"},
    SourceIconSpec{SourceIcon::background, "backgd.ico", "background_document"},
    SourceIconSpec{SourceIcon::ratings, "ratings.ico", "content_advisor_ratings"},
    SourceIconSpec{SourceIcon::whisper, "whisper.ico", "whisper_window"},
    SourceIconSpec{SourceIcon::notification, "notif.ico", "notification_connection"},
    SourceIconSpec{SourceIcon::connect_server, "tosrv.ico", "server_endpoint"},
    SourceIconSpec{SourceIcon::connect_network, "tonet.ico", "network_group"},
};

constexpr std::array strip_catalog{
    SourceStripSpec{SourceStrip::say_toolbar, "balloons.bmp", 118, 17, rgb_silver, say_cells},
    SourceStripSpec{SourceStrip::about_tiki, "tiki2.bmp", 490, 250, std::nullopt, tiki_cells},
    SourceStripSpec{SourceStrip::main_toolbar, "toolbar.bmp", 160, 16, rgb_silver, main_toolbar_cells},
    SourceStripSpec{SourceStrip::tabs, "tabbar.bmp", 64, 16, rgb_green, tab_cells},
    SourceStripSpec{SourceStrip::member_status, "member.bmp", 80, 16, rgb_blue, member_cells},
    SourceStripSpec{SourceStrip::old_new, "oldnew.bmp", 16, 16, rgb_blue, old_new_cells},
    SourceStripSpec{SourceStrip::connection, "connect.bmp", 32, 16, rgb_blue, connection_cells},
    SourceStripSpec{SourceStrip::rule_stopped, "stopped.bmp", 16, 15, std::nullopt, stopped_cells},
    SourceStripSpec{SourceStrip::rule_inactive, "inactive.bmp", 16, 15, std::nullopt, inactive_cells},
    SourceStripSpec{SourceStrip::rule_active, "active.bmp", 16, 15, std::nullopt, active_cells},
    SourceStripSpec{SourceStrip::text_toolbar, "texttool.bmp", 112, 16, rgb_silver, text_toolbar_cells},
    SourceStripSpec{SourceStrip::user_toolbar, "usertool.bmp", 112, 16, rgb_silver, user_toolbar_cells},
};

auto u16(const std::span<const std::byte> bytes, const std::size_t offset) -> std::optional<std::uint16_t> {
    if (offset > bytes.size() || bytes.size() - offset < 2) return std::nullopt;
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset])) |
           static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset + 1]) << 8U);
}

auto u32(const std::span<const std::byte> bytes, const std::size_t offset) -> std::optional<std::uint32_t> {
    if (offset > bytes.size() || bytes.size() - offset < 4) return std::nullopt;
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3])) << 24U);
}

auto s32(const std::span<const std::byte> bytes, const std::size_t offset) -> std::optional<std::int32_t> {
    const auto value = u32(bytes, offset);
    if (!value) return std::nullopt;
    return static_cast<std::int32_t>(*value);
}

auto range_fits(const std::size_t offset, const std::size_t length, const std::size_t size) -> bool {
    return offset <= size && length <= size - offset;
}

auto image_area(const std::uint32_t width, const std::uint32_t height) -> std::optional<std::size_t> {
    if (width == 0 || height == 0 || width > 16'384 || height > 16'384) return std::nullopt;
    const auto area = static_cast<std::uint64_t>(width) * height;
    if (area > std::numeric_limits<std::size_t>::max()) return std::nullopt;
    return static_cast<std::size_t>(area);
}

auto palette_from(
    const std::span<const std::byte> bytes,
    const std::size_t offset,
    const std::uint32_t count) -> std::expected<std::vector<std::uint32_t>, SourceRasterError> {
    if (count > 256 || !range_fits(offset, static_cast<std::size_t>(count) * 4U, bytes.size())) {
        return std::unexpected{SourceRasterError::invalid_stream};
    }
    std::vector<std::uint32_t> palette;
    palette.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto base = offset + static_cast<std::size_t>(index) * 4U;
        const auto blue = std::to_integer<std::uint32_t>(bytes[base]);
        const auto green = std::to_integer<std::uint32_t>(bytes[base + 1]);
        const auto red = std::to_integer<std::uint32_t>(bytes[base + 2]);
        palette.push_back((red << 16U) | (green << 8U) | blue);
    }
    return palette;
}

auto indexed_pixel(
    const std::span<const std::byte> row,
    const std::uint32_t x,
    const std::uint16_t bit_count) -> std::optional<std::uint32_t> {
    switch (bit_count) {
    case 1:
        return (std::to_integer<unsigned char>(row[x / 8U]) >> (7U - x % 8U)) & 1U;
    case 4: {
        const auto packed = std::to_integer<unsigned char>(row[x / 2U]);
        return x % 2U == 0 ? packed >> 4U : packed & 0x0fU;
    }
    case 8:
        return std::to_integer<unsigned char>(row[x]);
    default:
        return std::nullopt;
    }
}

auto decode_icon_dib(
    const std::span<const std::byte> bytes,
    const std::uint32_t directory_width,
    const std::uint32_t directory_height) -> std::expected<RasterImage, SourceRasterError> {
    const auto header_size = u32(bytes, 0);
    const auto signed_width = s32(bytes, 4);
    const auto signed_combined_height = s32(bytes, 8);
    const auto planes = u16(bytes, 12);
    const auto bit_count = u16(bytes, 14);
    const auto compression = u32(bytes, 16);
    const auto colors_used = u32(bytes, 32);
    if (!header_size || *header_size < 40 || !signed_width || !signed_combined_height ||
        !planes || *planes != 1 || !bit_count || !compression || *compression != 0 ||
        !colors_used || *signed_width <= 0 || *signed_combined_height == 0 ||
        *signed_combined_height == std::numeric_limits<std::int32_t>::min()) {
        return std::unexpected{SourceRasterError::invalid_stream};
    }
    if (*bit_count != 1 && *bit_count != 4 && *bit_count != 8 && *bit_count != 24 && *bit_count != 32) {
        return std::unexpected{SourceRasterError::unsupported_format};
    }
    const auto width = static_cast<std::uint32_t>(*signed_width);
    const auto combined_height = static_cast<std::uint32_t>(
        *signed_combined_height < 0 ? -*signed_combined_height : *signed_combined_height);
    if (combined_height % 2U != 0) return std::unexpected{SourceRasterError::invalid_stream};
    const auto height = combined_height / 2U;
    if (width != directory_width || height != directory_height || !image_area(width, height)) {
        return std::unexpected{SourceRasterError::invalid_stream};
    }

    const auto palette_count = *bit_count <= 8
        ? (*colors_used == 0 ? 1U << *bit_count : *colors_used)
        : 0U;
    const auto palette = palette_from(bytes, *header_size, palette_count);
    if (!palette) return std::unexpected{palette.error()};
    const auto xor_offset = static_cast<std::size_t>(*header_size) +
                            static_cast<std::size_t>(palette_count) * 4U;
    const auto xor_stride_64 = ((static_cast<std::uint64_t>(width) * *bit_count + 31U) / 32U) * 4U;
    const auto mask_stride_64 = ((static_cast<std::uint64_t>(width) + 31U) / 32U) * 4U;
    if (xor_stride_64 > std::numeric_limits<std::size_t>::max() ||
        mask_stride_64 > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{SourceRasterError::invalid_stream};
    }
    const auto xor_stride = static_cast<std::size_t>(xor_stride_64);
    const auto mask_stride = static_cast<std::size_t>(mask_stride_64);
    const auto xor_size = xor_stride * static_cast<std::size_t>(height);
    const auto mask_offset = xor_offset + xor_size;
    const auto mask_size = mask_stride * static_cast<std::size_t>(height);
    if (!range_fits(xor_offset, xor_size, bytes.size()) ||
        !range_fits(mask_offset, mask_size, bytes.size())) {
        return std::unexpected{SourceRasterError::invalid_stream};
    }

    bool has_source_alpha = false;
    if (*bit_count == 32) {
        for (std::uint32_t row = 0; row < height && !has_source_alpha; ++row) {
            const auto base = xor_offset + static_cast<std::size_t>(row) * xor_stride;
            for (std::uint32_t x = 0; x < width; ++x) {
                if (std::to_integer<unsigned char>(bytes[base + static_cast<std::size_t>(x) * 4U + 3U]) != 0) {
                    has_source_alpha = true;
                    break;
                }
            }
        }
    }

    RasterImage image{width, height, std::vector<std::uint32_t>(*image_area(width, height))};
    const bool top_down = *signed_combined_height < 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto source_y = top_down ? y : height - 1U - y;
        const auto xor_row_offset = xor_offset + static_cast<std::size_t>(source_y) * xor_stride;
        const auto mask_row_offset = mask_offset + static_cast<std::size_t>(source_y) * mask_stride;
        const auto xor_row = bytes.subspan(xor_row_offset, xor_stride);
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t rgb{};
            std::uint32_t alpha = 0xffU;
            if (*bit_count <= 8) {
                const auto index = indexed_pixel(xor_row, x, *bit_count);
                if (!index || *index >= palette->size()) {
                    return std::unexpected{SourceRasterError::invalid_stream};
                }
                rgb = (*palette)[*index];
            } else {
                const auto pixel_offset = xor_row_offset + static_cast<std::size_t>(x) * (*bit_count / 8U);
                const auto blue = std::to_integer<std::uint32_t>(bytes[pixel_offset]);
                const auto green = std::to_integer<std::uint32_t>(bytes[pixel_offset + 1]);
                const auto red = std::to_integer<std::uint32_t>(bytes[pixel_offset + 2]);
                rgb = (red << 16U) | (green << 8U) | blue;
                if (*bit_count == 32 && has_source_alpha) {
                    alpha = std::to_integer<std::uint32_t>(bytes[pixel_offset + 3]);
                }
            }
            const auto mask_byte = std::to_integer<unsigned char>(
                bytes[mask_row_offset + static_cast<std::size_t>(x / 8U)]);
            if (((mask_byte >> (7U - x % 8U)) & 1U) != 0U) alpha = 0;
            image.argb[static_cast<std::size_t>(y) * width + x] = (alpha << 24U) | rgb;
        }
    }
    return image;
}

auto decode_rle4(
    const std::span<const std::byte> encoded,
    const std::uint32_t width,
    const std::uint32_t height) -> std::expected<std::vector<std::uint8_t>, SourceRasterError> {
    const auto area = image_area(width, height);
    if (!area) return std::unexpected{SourceRasterError::invalid_stream};
    std::vector<std::uint8_t> indices(*area);
    std::size_t offset{};
    std::uint32_t x{};
    std::uint32_t y{};
    bool ended = false;
    while (offset < encoded.size() && !ended) {
        if (!range_fits(offset, 2, encoded.size())) {
            return std::unexpected{SourceRasterError::invalid_stream};
        }
        const auto count = std::to_integer<std::uint32_t>(encoded[offset++]);
        const auto value = std::to_integer<std::uint32_t>(encoded[offset++]);
        if (count != 0) {
            if (y >= height || count > width - x) return std::unexpected{SourceRasterError::invalid_stream};
            for (std::uint32_t index = 0; index < count; ++index) {
                indices[static_cast<std::size_t>(y) * width + x++] = static_cast<std::uint8_t>(
                    index % 2U == 0 ? value >> 4U : value & 0x0fU);
            }
            continue;
        }
        if (value == 0) {
            x = 0;
            if (y < height) ++y;
        } else if (value == 1) {
            ended = true;
        } else if (value == 2) {
            if (!range_fits(offset, 2, encoded.size())) {
                return std::unexpected{SourceRasterError::invalid_stream};
            }
            const auto dx = std::to_integer<std::uint32_t>(encoded[offset++]);
            const auto dy = std::to_integer<std::uint32_t>(encoded[offset++]);
            if (dx > width - x || dy > height - std::min(y, height)) {
                return std::unexpected{SourceRasterError::invalid_stream};
            }
            x += dx;
            y += dy;
        } else {
            const auto absolute_count = value;
            const auto packed_count = (absolute_count + 1U) / 2U;
            const auto padded_count = packed_count + (packed_count & 1U);
            if (y >= height || absolute_count > width - x ||
                !range_fits(offset, padded_count, encoded.size())) {
                return std::unexpected{SourceRasterError::invalid_stream};
            }
            for (std::uint32_t index = 0; index < absolute_count; ++index) {
                const auto packed = std::to_integer<std::uint32_t>(encoded[offset + index / 2U]);
                indices[static_cast<std::size_t>(y) * width + x++] = static_cast<std::uint8_t>(
                    index % 2U == 0 ? packed >> 4U : packed & 0x0fU);
            }
            offset += padded_count;
        }
    }
    if (!ended) return std::unexpected{SourceRasterError::invalid_stream};
    return indices;
}

auto read_source_file(const std::filesystem::path& path)
    -> std::expected<std::vector<std::byte>, SourceRasterError> {
    try {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return std::unexpected{SourceRasterError::io};
        const auto end = file.tellg();
        if (end <= 0 || static_cast<std::uintmax_t>(end) > maximum_source_file_size) {
            return std::unexpected{SourceRasterError::invalid_stream};
        }
        std::vector<std::byte> bytes(static_cast<std::size_t>(end));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) return std::unexpected{SourceRasterError::io};
        return bytes;
    } catch (const std::bad_alloc&) {
        return std::unexpected{SourceRasterError::allocation};
    } catch (const std::filesystem::filesystem_error&) {
        return std::unexpected{SourceRasterError::io};
    }
}

auto is_source_directory(const std::filesystem::path& path) -> bool {
    std::error_code error;
    return std::filesystem::is_regular_file(path / "chat.ico", error) &&
           std::filesystem::is_regular_file(path / "toolbar.bmp", error) &&
           std::filesystem::is_regular_file(path / "balloons.bmp", error);
}

} // namespace

auto source_icon_catalog() noexcept -> std::span<const SourceIconSpec> { return icon_catalog; }
auto source_strip_catalog() noexcept -> std::span<const SourceStripSpec> { return strip_catalog; }

auto source_icon_spec(const SourceIcon id) noexcept -> const SourceIconSpec& {
    return icon_catalog[static_cast<std::size_t>(id)];
}

auto source_strip_spec(const SourceStrip id) noexcept -> const SourceStripSpec& {
    return strip_catalog[static_cast<std::size_t>(id)];
}

auto decode_windows_icon(const std::span<const std::byte> bytes, const std::uint32_t preferred_width)
    -> std::expected<RasterImage, SourceRasterError> {
    try {
        const auto reserved = u16(bytes, 0);
        const auto type = u16(bytes, 2);
        const auto count = u16(bytes, 4);
        if (!reserved || *reserved != 0 || !type || *type != 1 || !count || *count == 0 ||
            !range_fits(6, static_cast<std::size_t>(*count) * 16U, bytes.size())) {
            return std::unexpected{SourceRasterError::invalid_stream};
        }
        std::optional<std::size_t> selected;
        std::uint32_t selected_distance = std::numeric_limits<std::uint32_t>::max();
        std::uint16_t selected_depth{};
        std::uint32_t selected_width{};
        for (std::uint16_t index = 0; index < *count; ++index) {
            const auto entry = 6U + static_cast<std::size_t>(index) * 16U;
            const auto width_byte = std::to_integer<std::uint32_t>(bytes[entry]);
            const auto height_byte = std::to_integer<std::uint32_t>(bytes[entry + 1]);
            const auto width = width_byte == 0 ? 256U : width_byte;
            const auto height = height_byte == 0 ? 256U : height_byte;
            const auto depth = u16(bytes, entry + 6);
            const auto data_size = u32(bytes, entry + 8);
            const auto data_offset = u32(bytes, entry + 12);
            if (!depth || !data_size || !data_offset || width != height ||
                !range_fits(*data_offset, *data_size, bytes.size())) continue;
            // The two connection ICOs leave ICONDIRENTRY::wBitCount at zero.
            // Their DIB headers are authoritative: select the authored 8-bit
            // frame instead of whichever 4-bit fallback happens to come first.
            const auto dib_depth = u16(bytes, static_cast<std::size_t>(*data_offset) + 14U);
            const auto effective_depth = dib_depth.value_or(*depth);
            const auto distance = preferred_width == 0
                ? 0U
                : (width > preferred_width ? width - preferred_width : preferred_width - width);
            const bool better = !selected || distance < selected_distance ||
                (distance == selected_distance && effective_depth > selected_depth) ||
                (distance == selected_distance && effective_depth == selected_depth && width > selected_width);
            if (better) {
                selected = entry;
                selected_distance = distance;
                selected_depth = effective_depth;
                selected_width = width;
            }
        }
        if (!selected) return std::unexpected{SourceRasterError::invalid_stream};
        const auto width_byte = std::to_integer<std::uint32_t>(bytes[*selected]);
        const auto height_byte = std::to_integer<std::uint32_t>(bytes[*selected + 1]);
        const auto width = width_byte == 0 ? 256U : width_byte;
        const auto height = height_byte == 0 ? 256U : height_byte;
        const auto data_size = *u32(bytes, *selected + 8);
        const auto data_offset = *u32(bytes, *selected + 12);
        return decode_icon_dib(bytes.subspan(data_offset, data_size), width, height);
    } catch (const std::bad_alloc&) {
        return std::unexpected{SourceRasterError::allocation};
    }
}

auto decode_windows_bitmap(const std::span<const std::byte> bytes)
    -> std::expected<RasterImage, SourceRasterError> {
    try {
        if (bytes.size() < 54 || std::to_integer<unsigned char>(bytes[0]) != 'B' ||
            std::to_integer<unsigned char>(bytes[1]) != 'M') {
            return std::unexpected{SourceRasterError::invalid_stream};
        }
        const auto pixel_offset = u32(bytes, 10);
        const auto header_size = u32(bytes, 14);
        const auto signed_width = s32(bytes, 18);
        const auto signed_height = s32(bytes, 22);
        const auto planes = u16(bytes, 26);
        const auto bit_count = u16(bytes, 28);
        const auto compression = u32(bytes, 30);
        const auto colors_used = u32(bytes, 46);
        if (!pixel_offset || !header_size || *header_size < 40 || !signed_width || *signed_width <= 0 ||
            !signed_height || *signed_height == 0 || *signed_height == std::numeric_limits<std::int32_t>::min() ||
            !planes || *planes != 1 || !bit_count || !compression || !colors_used) {
            return std::unexpected{SourceRasterError::invalid_stream};
        }
        if (*bit_count != 1 && *bit_count != 4 && *bit_count != 8 && *bit_count != 24 && *bit_count != 32) {
            return std::unexpected{SourceRasterError::unsupported_format};
        }
        if (*compression != 0 && !(*compression == 2 && *bit_count == 4 && *signed_height > 0)) {
            return std::unexpected{SourceRasterError::unsupported_format};
        }
        const auto width = static_cast<std::uint32_t>(*signed_width);
        const auto height = static_cast<std::uint32_t>(*signed_height < 0 ? -*signed_height : *signed_height);
        const auto area = image_area(width, height);
        if (!area || *pixel_offset > bytes.size()) return std::unexpected{SourceRasterError::invalid_stream};
        const auto palette_count = *bit_count <= 8
            ? (*colors_used == 0 ? 1U << *bit_count : *colors_used)
            : 0U;
        const auto palette = palette_from(bytes, 14U + *header_size, palette_count);
        if (!palette) return std::unexpected{palette.error()};
        RasterImage image{width, height, std::vector<std::uint32_t>(*area)};

        if (*compression == 2) {
            const auto indices = decode_rle4(bytes.subspan(*pixel_offset), width, height);
            if (!indices) return std::unexpected{indices.error()};
            for (std::uint32_t y = 0; y < height; ++y) {
                const auto source_y = height - 1U - y;
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto index = (*indices)[static_cast<std::size_t>(source_y) * width + x];
                    if (index >= palette->size()) return std::unexpected{SourceRasterError::invalid_stream};
                    image.argb[static_cast<std::size_t>(y) * width + x] = 0xff00'0000U | (*palette)[index];
                }
            }
            return image;
        }

        const auto stride_64 = ((static_cast<std::uint64_t>(width) * *bit_count + 31U) / 32U) * 4U;
        if (stride_64 > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected{SourceRasterError::invalid_stream};
        }
        const auto stride = static_cast<std::size_t>(stride_64);
        if (height > 0 && stride > std::numeric_limits<std::size_t>::max() / height) {
            return std::unexpected{SourceRasterError::invalid_stream};
        }
        if (!range_fits(*pixel_offset, stride * static_cast<std::size_t>(height), bytes.size())) {
            return std::unexpected{SourceRasterError::invalid_stream};
        }
        const bool top_down = *signed_height < 0;
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto source_y = top_down ? y : height - 1U - y;
            const auto row_offset = *pixel_offset + static_cast<std::size_t>(source_y) * stride;
            const auto row = bytes.subspan(row_offset, stride);
            for (std::uint32_t x = 0; x < width; ++x) {
                std::uint32_t rgb{};
                if (*bit_count <= 8) {
                    const auto index = indexed_pixel(row, x, *bit_count);
                    if (!index || *index >= palette->size()) {
                        return std::unexpected{SourceRasterError::invalid_stream};
                    }
                    rgb = (*palette)[*index];
                } else {
                    const auto offset = row_offset + static_cast<std::size_t>(x) * (*bit_count / 8U);
                    const auto blue = std::to_integer<std::uint32_t>(bytes[offset]);
                    const auto green = std::to_integer<std::uint32_t>(bytes[offset + 1]);
                    const auto red = std::to_integer<std::uint32_t>(bytes[offset + 2]);
                    rgb = (red << 16U) | (green << 8U) | blue;
                }
                image.argb[static_cast<std::size_t>(y) * width + x] = 0xff00'0000U | rgb;
            }
        }
        return image;
    } catch (const std::bad_alloc&) {
        return std::unexpected{SourceRasterError::allocation};
    }
}

auto load_source_icon(
    const std::filesystem::path& source_directory,
    const SourceIcon id,
    const std::uint32_t preferred_width) -> std::expected<RasterImage, SourceRasterError> {
    const auto bytes = read_source_file(source_directory / source_icon_spec(id).file_name);
    if (!bytes) return std::unexpected{bytes.error()};
    return decode_windows_icon(*bytes, preferred_width);
}

auto load_source_strip(const std::filesystem::path& source_directory, const SourceStrip id)
    -> std::expected<RasterImage, SourceRasterError> {
    const auto& spec = source_strip_spec(id);
    const auto bytes = read_source_file(source_directory / spec.file_name);
    if (!bytes) return std::unexpected{bytes.error()};
    auto image = decode_windows_bitmap(*bytes);
    if (!image) return image;
    if (image->width != spec.width || image->height != spec.height) {
        return std::unexpected{SourceRasterError::dimension_mismatch};
    }
    if (spec.transparent_rgb) {
        for (auto& pixel : image->argb) {
            if ((pixel & 0x00ff'ffffU) == *spec.transparent_rgb) pixel &= 0x00ff'ffffU;
        }
    }
    return image;
}

auto find_source_raster_directory()
    -> std::expected<std::filesystem::path, SourceRasterError> {
    try {
        if (const char* override_path = std::getenv("COMICCHAT_SOURCE_RASTER_DIR");
            override_path != nullptr && *override_path != '\0') {
            const std::filesystem::path candidate{override_path};
            if (is_source_directory(candidate)) return candidate;
        }
#ifdef COMICCHAT_INSTALL_SOURCE_RASTER_DIR
        if (const std::filesystem::path candidate{COMICCHAT_INSTALL_SOURCE_RASTER_DIR};
            is_source_directory(candidate)) return candidate;
#endif
#ifdef COMICCHAT_SOURCE_RASTER_DIR
        if (const std::filesystem::path candidate{COMICCHAT_SOURCE_RASTER_DIR};
            is_source_directory(candidate)) return candidate;
#endif
        return std::unexpected{SourceRasterError::source_directory_missing};
    } catch (const std::bad_alloc&) {
        return std::unexpected{SourceRasterError::allocation};
    } catch (const std::filesystem::filesystem_error&) {
        return std::unexpected{SourceRasterError::io};
    }
}

} // namespace comicchat
