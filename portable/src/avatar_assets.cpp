#include "comicchat/avatar_assets.hpp"

#include "comicchat/assets.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <cmath>
#include <new>
#include <span>
#include <utility>

#include <cairo.h>

namespace comicchat {
namespace {

constexpr std::uint16_t magic_old = 0x0081;
constexpr std::uint16_t magic_new = 0x8181;
constexpr std::uint16_t current_version = 2;
constexpr std::uint16_t tag_name = 1;
constexpr std::uint16_t tag_flags = 2;
constexpr std::uint16_t tag_icon = 3;
constexpr std::uint16_t tag_faces_old = 4;
constexpr std::uint16_t tag_torsos_old = 5;
constexpr std::uint16_t tag_start_data = 6;
constexpr std::uint16_t tag_style = 8;
constexpr std::uint16_t tag_bodies_old = 9;
constexpr std::uint16_t tag_faces = 10;
constexpr std::uint16_t tag_torsos = 11;
constexpr std::uint16_t tag_bodies = 12;
constexpr std::uint16_t tag_icon_new = 256;
constexpr std::uint16_t tag_palette = 257;
constexpr std::uint16_t tag_backdrop = 258;
constexpr std::uint16_t tag_copyright = 259;
constexpr std::uint16_t tag_original_url = 260;
constexpr std::uint16_t tag_override_url = 261;
constexpr std::uint16_t tag_usage_flags = 262;
constexpr std::uint16_t tag_offset_adjustment = 263;
constexpr std::size_t maximum_file_bytes = 128U * 1024U * 1024U;
constexpr std::size_t maximum_image_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_components = 4096;
constexpr std::size_t maximum_palette = 2048;
constexpr std::int32_t maximum_dimension = 32768;

enum class ImageFormat : std::uint8_t { dib = 0, zlib = 1 };
enum class PaletteType : std::uint8_t {
    none = 0,
    global = 1,
    local = 2,
    monochrome = 3,
    masked_mono = 4,
    dual_mask = 5,
};

struct ImageRef final {
    std::uint32_t offset{};
    ImageFormat format{};
    PaletteType palette{};
};

struct PoseRef final {
    std::array<ImageRef, 3> images;
};

struct DecodedBitmap final {
    AvatarBitmap bitmap;
    std::vector<std::uint8_t> indices;
    std::uint16_t bits_per_pixel{};
};

class Reader final {
public:
    explicit Reader(std::span<const std::byte> bytes, std::size_t position = 0) : bytes_{bytes}, position_{position} {}

    [[nodiscard]] auto position() const noexcept -> std::size_t { return position_; }
    [[nodiscard]] auto remaining() const noexcept -> std::size_t {
        return position_ <= bytes_.size() ? bytes_.size() - position_ : 0;
    }
    [[nodiscard]] auto seek(const std::size_t position) noexcept -> bool {
        if (position > bytes_.size()) return false;
        position_ = position;
        return true;
    }
    [[nodiscard]] auto skip(const std::size_t size) noexcept -> bool {
        if (size > remaining()) return false;
        position_ += size;
        return true;
    }
    [[nodiscard]] auto bytes(const std::size_t size) noexcept -> std::optional<std::span<const std::byte>> {
        if (size > remaining()) return std::nullopt;
        const auto result = bytes_.subspan(position_, size);
        position_ += size;
        return result;
    }
    [[nodiscard]] auto u8() noexcept -> std::optional<std::uint8_t> {
        const auto value = bytes(1);
        if (!value) return std::nullopt;
        return std::to_integer<std::uint8_t>((*value)[0]);
    }
    [[nodiscard]] auto u16() noexcept -> std::optional<std::uint16_t> {
        const auto value = bytes(2);
        if (!value) return std::nullopt;
        return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>((*value)[0])) |
            static_cast<std::uint16_t>(std::to_integer<std::uint8_t>((*value)[1]) << 8U);
    }
    [[nodiscard]] auto i16() noexcept -> std::optional<std::int16_t> {
        const auto value = u16();
        if (!value) return std::nullopt;
        return std::bit_cast<std::int16_t>(*value);
    }
    [[nodiscard]] auto u32() noexcept -> std::optional<std::uint32_t> {
        const auto value = bytes(4);
        if (!value) return std::nullopt;
        return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*value)[0])) |
            (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*value)[1])) << 8U) |
            (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*value)[2])) << 16U) |
            (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>((*value)[3])) << 24U);
    }
    [[nodiscard]] auto i32() noexcept -> std::optional<std::int32_t> {
        const auto value = u32();
        if (!value) return std::nullopt;
        return std::bit_cast<std::int32_t>(*value);
    }
    [[nodiscard]] auto string(const std::size_t maximum) -> std::optional<std::string> {
        std::string result;
        result.reserve(std::min(maximum, remaining()));
        while (result.size() + 1 < maximum) {
            const auto value = u8();
            if (!value) return std::nullopt;
            if (*value == 0) return result;
            result.push_back(static_cast<char>(*value));
        }
        return std::nullopt;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t position_{};
};

[[nodiscard]] auto checked_stride(const std::int32_t width, const std::uint16_t bits_per_pixel)
    -> std::optional<std::size_t> {
    if (width <= 0 || width > maximum_dimension) return std::nullopt;
    const auto bits = static_cast<std::uint64_t>(width) * bits_per_pixel;
    const auto stride = ((bits + 31U) / 32U) * 4U;
    if (stride == 0 || stride > maximum_image_bytes) return std::nullopt;
    return static_cast<std::size_t>(stride);
}

[[nodiscard]] auto color(const std::uint8_t red, const std::uint8_t green, const std::uint8_t blue)
    -> std::uint32_t {
    return 0xff000000U | (static_cast<std::uint32_t>(red) << 16U) |
        (static_cast<std::uint32_t>(green) << 8U) | blue;
}

[[nodiscard]] auto read_palette(Reader& reader) -> std::expected<std::vector<std::uint32_t>, AvatarAssetError> {
    const auto count = reader.u16();
    if (!count) return std::unexpected{AvatarAssetError::truncated};
    if (*count > maximum_palette) return std::unexpected{AvatarAssetError::invalid_palette};
    std::vector<std::uint32_t> palette;
    palette.reserve(*count);
    for (std::uint16_t index = 0; index < *count; ++index) {
        // AVB palette triples are serialized in the DIB table's B,G,R order.
        // The Microsoft reader stores the triple through COLORREF/RGBQUAD
        // conversion before drawing; treating byte zero as red swaps every
        // colored character's ink (blue hair becomes orange, for example).
        const auto blue = reader.u8();
        const auto green = reader.u8();
        const auto red = reader.u8();
        if (!blue || !green || !red) return std::unexpected{AvatarAssetError::truncated};
        palette.push_back(color(*red, *green, *blue));
    }
    return palette;
}

[[nodiscard]] auto decode_pixels(
    const std::span<const std::byte> bits,
    const std::int32_t width,
    const std::int32_t signed_height,
    const std::uint16_t bits_per_pixel,
    const std::uint32_t compression,
    const std::span<const std::uint32_t> palette) -> std::expected<DecodedBitmap, AvatarAssetError> {
    if (signed_height == 0 || signed_height == std::numeric_limits<std::int32_t>::min())
        return std::unexpected{AvatarAssetError::invalid_bitmap};
    const auto height = signed_height < 0 ? -signed_height : signed_height;
    if (height > maximum_dimension || compression != 0)
        return std::unexpected{compression == 0 ? AvatarAssetError::invalid_bitmap : AvatarAssetError::unsupported_format};
    if (bits_per_pixel != 1 && bits_per_pixel != 2 && bits_per_pixel != 4 && bits_per_pixel != 8 &&
        bits_per_pixel != 16 && bits_per_pixel != 24 && bits_per_pixel != 32)
        return std::unexpected{AvatarAssetError::unsupported_format};
    const auto stride = checked_stride(width, bits_per_pixel);
    if (!stride) return std::unexpected{AvatarAssetError::invalid_bitmap};
    const auto required = static_cast<std::uint64_t>(*stride) * static_cast<std::uint64_t>(height);
    if (required == 0 || required > maximum_image_bytes || required > bits.size())
        return std::unexpected{AvatarAssetError::invalid_bitmap};
    const auto pixels_count = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixels_count > maximum_image_bytes / sizeof(std::uint32_t))
        return std::unexpected{AvatarAssetError::invalid_bitmap};

    DecodedBitmap result;
    result.bitmap.width = width;
    result.bitmap.height = height;
    result.bits_per_pixel = bits_per_pixel;
    result.bitmap.pixels.resize(static_cast<std::size_t>(pixels_count));
    if (bits_per_pixel <= 8) result.indices.resize(static_cast<std::size_t>(pixels_count));

    for (std::int32_t y = 0; y < height; ++y) {
        const auto source_y = signed_height > 0 ? height - y - 1 : y;
        const auto row = bits.subspan(static_cast<std::size_t>(source_y) * *stride, *stride);
        for (std::int32_t x = 0; x < width; ++x) {
            std::uint32_t pixel{};
            std::uint8_t palette_index{};
            switch (bits_per_pixel) {
            case 1:
                palette_index = (std::to_integer<std::uint8_t>(row[static_cast<std::size_t>(x) / 8U]) >>
                    (7U - (static_cast<unsigned>(x) % 8U))) & 1U;
                break;
            case 2:
                palette_index = (std::to_integer<std::uint8_t>(row[static_cast<std::size_t>(x) / 4U]) >>
                    (6U - (static_cast<unsigned>(x) % 4U) * 2U)) & 3U;
                break;
            case 4:
                palette_index = (std::to_integer<std::uint8_t>(row[static_cast<std::size_t>(x) / 2U]) >>
                    (static_cast<unsigned>(x) % 2U == 0 ? 4U : 0U)) & 15U;
                break;
            case 8:
                palette_index = std::to_integer<std::uint8_t>(row[static_cast<std::size_t>(x)]);
                break;
            case 16: {
                const auto offset = static_cast<std::size_t>(x) * 2U;
                const auto packed = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(row[offset])) |
                    static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(row[offset + 1U]) << 8U);
                const auto red = static_cast<std::uint8_t>(((packed >> 10U) & 31U) * 255U / 31U);
                const auto green = static_cast<std::uint8_t>(((packed >> 5U) & 31U) * 255U / 31U);
                const auto blue = static_cast<std::uint8_t>((packed & 31U) * 255U / 31U);
                pixel = color(red, green, blue);
                break;
            }
            case 24: {
                const auto offset = static_cast<std::size_t>(x) * 3U;
                pixel = color(std::to_integer<std::uint8_t>(row[offset + 2U]),
                    std::to_integer<std::uint8_t>(row[offset + 1U]),
                    std::to_integer<std::uint8_t>(row[offset]));
                break;
            }
            case 32: {
                const auto offset = static_cast<std::size_t>(x) * 4U;
                pixel = color(std::to_integer<std::uint8_t>(row[offset + 2U]),
                    std::to_integer<std::uint8_t>(row[offset + 1U]),
                    std::to_integer<std::uint8_t>(row[offset]));
                break;
            }
            default:
                return std::unexpected{AvatarAssetError::unsupported_format};
            }
            const auto destination = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x);
            if (bits_per_pixel <= 8) {
                if (palette_index >= palette.size()) return std::unexpected{AvatarAssetError::invalid_palette};
                result.indices[destination] = palette_index;
                pixel = palette[palette_index];
            }
            result.bitmap.pixels[destination] = pixel;
        }
    }
    return result;
}

struct DibHeader final {
    std::uint32_t size{};
    std::int32_t width{};
    std::int32_t height{};
    std::uint16_t planes{};
    std::uint16_t bits_per_pixel{};
    std::uint32_t compression{};
    std::uint32_t image_size{};
    std::uint32_t colors_used{};
};

[[nodiscard]] auto read_dib_header(Reader& reader) -> std::expected<DibHeader, AvatarAssetError> {
    const auto size = reader.u32();
    if (!size) return std::unexpected{AvatarAssetError::truncated};
    if (*size < 40 || *size > 240) return std::unexpected{AvatarAssetError::unsupported_format};
    const auto rest = reader.bytes(*size - 4U);
    if (!rest) return std::unexpected{AvatarAssetError::truncated};
    Reader header{*rest};
    const auto width = header.i32();
    const auto height = header.i32();
    const auto planes = header.u16();
    const auto bits_per_pixel = header.u16();
    const auto compression = header.u32();
    const auto image_size = header.u32();
    if (!width || !height || !planes || !bits_per_pixel || !compression || !image_size)
        return std::unexpected{AvatarAssetError::truncated};
    if (!header.skip(8)) return std::unexpected{AvatarAssetError::truncated};
    const auto colors_used = header.u32();
    if (!colors_used) return std::unexpected{AvatarAssetError::truncated};
    if (*planes != 1 || *width <= 0 || *width > maximum_dimension || *height == 0)
        return std::unexpected{AvatarAssetError::invalid_bitmap};
    return DibHeader{*size, *width, *height, *planes, *bits_per_pixel, *compression, *image_size, *colors_used};
}

[[nodiscard]] auto standard_palette(const PaletteType type, Reader& reader,
    const std::span<const std::uint32_t> global_palette)
    -> std::expected<std::vector<std::uint32_t>, AvatarAssetError> {
    switch (type) {
    case PaletteType::global:
        if (global_palette.empty()) return std::unexpected{AvatarAssetError::invalid_palette};
        return std::vector<std::uint32_t>{global_palette.begin(), global_palette.end()};
    case PaletteType::local: {
        const auto tag = reader.u16();
        const auto size = reader.u16();
        if (!tag || !size) return std::unexpected{AvatarAssetError::truncated};
        if (*tag != tag_palette) return std::unexpected{AvatarAssetError::invalid_palette};
        const auto start = reader.position();
        auto palette = read_palette(reader);
        if (!palette) return palette;
        if (reader.position() - start > *size) return std::unexpected{AvatarAssetError::invalid_palette};
        if (!reader.skip(*size - (reader.position() - start))) return std::unexpected{AvatarAssetError::truncated};
        return palette;
    }
    case PaletteType::monochrome:
        return std::vector<std::uint32_t>{color(255, 255, 255), color(0, 0, 0)};
    case PaletteType::masked_mono:
    case PaletteType::dual_mask:
        return std::vector<std::uint32_t>{
            color(255, 255, 255), color(0, 0, 0), color(128, 0, 0), color(0, 0, 128)};
    case PaletteType::none:
        return std::vector<std::uint32_t>{};
    }
    return std::unexpected{AvatarAssetError::invalid_palette};
}

[[nodiscard]] auto decode_zlib_image(Reader& reader, const PaletteType palette_type,
    const std::span<const std::uint32_t> global_palette) -> std::expected<DecodedBitmap, AvatarAssetError> {
    auto palette = standard_palette(palette_type, reader, global_palette);
    if (!palette) return std::unexpected{palette.error()};
    if (palette_type == PaletteType::none) return std::unexpected{AvatarAssetError::invalid_palette};
    auto header = read_dib_header(reader);
    if (!header) return std::unexpected{header.error()};
    const auto uncompressed_size = reader.u32();
    const auto compressed_size = reader.u32();
    if (!uncompressed_size || !compressed_size) return std::unexpected{AvatarAssetError::truncated};
    if (*uncompressed_size == 0 || *compressed_size == 0 || *uncompressed_size > maximum_image_bytes ||
        *compressed_size > maximum_image_bytes)
        return std::unexpected{AvatarAssetError::invalid_bitmap};
    const auto compressed = reader.bytes(*compressed_size);
    if (!compressed) return std::unexpected{AvatarAssetError::truncated};
    auto inflated = inflate_asset(*compressed, *uncompressed_size);
    if (!inflated || inflated->size() != *uncompressed_size)
        return std::unexpected{AvatarAssetError::decompression};
    return decode_pixels(*inflated, header->width, header->height, header->bits_per_pixel,
        header->compression, *palette);
}

[[nodiscard]] auto decode_dib_image(Reader& reader) -> std::expected<DecodedBitmap, AvatarAssetError> {
    const auto resource_start = reader.position();
    const auto signature = reader.u16();
    const auto file_size = reader.u32();
    if (!signature || !file_size) return std::unexpected{AvatarAssetError::truncated};
    if (*signature != 0x4d42 || *file_size < 54 || *file_size > reader.remaining() + 6U)
        return std::unexpected{AvatarAssetError::invalid_bitmap};
    if (!reader.skip(4)) return std::unexpected{AvatarAssetError::truncated};
    const auto bits_offset = reader.u32();
    if (!bits_offset || *bits_offset >= *file_size) return std::unexpected{AvatarAssetError::invalid_bitmap};
    auto header = read_dib_header(reader);
    if (!header) return std::unexpected{header.error()};
    std::vector<std::uint32_t> palette;
    if (header->bits_per_pixel <= 8) {
        const auto default_count = std::uint32_t{1} << header->bits_per_pixel;
        const auto count = header->colors_used == 0 ? default_count : header->colors_used;
        if (count > maximum_palette) return std::unexpected{AvatarAssetError::invalid_palette};
        palette.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto blue = reader.u8();
            const auto green = reader.u8();
            const auto red = reader.u8();
            const auto reserved = reader.u8();
            if (!blue || !green || !red || !reserved) return std::unexpected{AvatarAssetError::truncated};
            palette.push_back(color(*red, *green, *blue));
        }
    }
    const auto bits_position = static_cast<std::uint64_t>(resource_start) + *bits_offset;
    const auto end_position = static_cast<std::uint64_t>(resource_start) + *file_size;
    if (bits_position > end_position || end_position > resource_start + reader.remaining() + (reader.position() - resource_start))
        return std::unexpected{AvatarAssetError::invalid_bitmap};
    if (!reader.seek(static_cast<std::size_t>(bits_position))) return std::unexpected{AvatarAssetError::invalid_offset};
    const auto bits = reader.bytes(static_cast<std::size_t>(end_position - bits_position));
    if (!bits) return std::unexpected{AvatarAssetError::truncated};
    return decode_pixels(*bits, header->width, header->height, header->bits_per_pixel,
        header->compression, palette);
}

class AssetParser final {
public:
    explicit AssetParser(std::span<const std::byte> bytes) : bytes_{bytes}, reader_{bytes} {}

    [[nodiscard]] auto parse() -> std::expected<AvatarAsset, AvatarAssetError> {
        const auto magic = reader_.u16();
        const auto type = reader_.u16();
        const auto version = reader_.u16();
        if (!magic || !type || !version) return std::unexpected{AvatarAssetError::truncated};
        if ((*magic != magic_old && *magic != magic_new) || *version != current_version ||
            *type < static_cast<std::uint16_t>(AvatarKind::simple) ||
            *type > static_cast<std::uint16_t>(AvatarKind::backdrop))
            return std::unexpected{AvatarAssetError::invalid_header};
        asset_.kind = static_cast<AvatarKind>(*type);
        if (asset_.kind == AvatarKind::backdrop) return parse_backdrop();
        auto records = parse_avatar_records();
        if (!records) return std::unexpected{records.error()};
        auto decoded = decode_poses();
        if (!decoded) return std::unexpected{decoded.error()};
        return std::move(asset_);
    }

private:
    [[nodiscard]] auto adjusted_offset(const std::uint32_t raw) const -> std::optional<std::uint32_t> {
        if (raw == 0) return 0;
        const auto adjusted = static_cast<std::int64_t>(raw) + resource_adjustment_;
        if (adjusted <= 0 || adjusted >= static_cast<std::int64_t>(bytes_.size()) ||
            adjusted > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
        return static_cast<std::uint32_t>(adjusted);
    }

    [[nodiscard]] auto image_ref(const std::uint32_t raw_offset, const std::uint8_t raw_format,
        const std::uint8_t raw_palette) const -> std::optional<ImageRef> {
        const auto offset = adjusted_offset(raw_offset);
        if (!offset || raw_format > static_cast<std::uint8_t>(ImageFormat::zlib) ||
            raw_palette > static_cast<std::uint8_t>(PaletteType::dual_mask)) return std::nullopt;
        return ImageRef{*offset, static_cast<ImageFormat>(raw_format), static_cast<PaletteType>(raw_palette)};
    }

    [[nodiscard]] auto add_pose(const std::array<std::uint32_t, 3>& offsets,
        const std::array<std::uint8_t, 3>& formats, const std::array<std::uint8_t, 3>& palettes)
        -> std::expected<std::uint16_t, AvatarAssetError> {
        if (pose_refs_.size() >= maximum_components || offsets[0] == 0)
            return std::unexpected{AvatarAssetError::invalid_record};
        PoseRef pose;
        for (std::size_t index = 0; index < pose.images.size(); ++index) {
            if (offsets[index] == 0) continue;
            const auto ref = image_ref(offsets[index], formats[index], palettes[index]);
            if (!ref) return std::unexpected{AvatarAssetError::invalid_record};
            pose.images[index] = *ref;
        }
        pose_refs_.push_back(pose);
        return static_cast<std::uint16_t>(pose_refs_.size());
    }

    [[nodiscard]] auto read_new_size(const std::uint16_t tag) -> std::expected<std::uint16_t, AvatarAssetError> {
        if (tag < tag_icon_new) return 0;
        const auto size = reader_.u16();
        if (!size) return std::unexpected{AvatarAssetError::truncated};
        if (*size > reader_.remaining()) return std::unexpected{AvatarAssetError::truncated};
        return *size;
    }

    [[nodiscard]] auto parse_avatar_records() -> std::expected<void, AvatarAssetError> {
        for (;;) {
            const auto tag = reader_.u16();
            if (!tag) return std::unexpected{AvatarAssetError::truncated};
            auto size = read_new_size(*tag);
            if (!size) return std::unexpected{size.error()};
            const auto payload_start = reader_.position();
            if (*tag == tag_start_data) break;
            switch (*tag) {
            case tag_name: {
                auto value = reader_.string(60);
                if (!value) return std::unexpected{AvatarAssetError::invalid_record};
                asset_.name = std::move(*value);
                break;
            }
            case tag_flags: {
                const auto value = reader_.u16();
                if (!value) return std::unexpected{AvatarAssetError::truncated};
                asset_.flags = static_cast<std::uint8_t>(*value);
                break;
            }
            case tag_style: {
                const auto value = reader_.u16();
                if (!value) return std::unexpected{AvatarAssetError::truncated};
                asset_.style = static_cast<std::uint8_t>(*value);
                break;
            }
            case tag_icon:
            case tag_icon_new: {
                const auto offset = reader_.u32();
                if (!offset) return std::unexpected{AvatarAssetError::truncated};
                std::uint8_t format{};
                std::uint8_t palette{};
                if (*tag == tag_icon_new) {
                    const auto raw_format = reader_.u8();
                    const auto raw_palette = reader_.u8();
                    if (!raw_format || !raw_palette) return std::unexpected{AvatarAssetError::truncated};
                    format = *raw_format;
                    palette = *raw_palette;
                }
                auto pose_id = add_pose({*offset, 0, 0}, {format, 0, 0}, {palette, 0, 0});
                if (!pose_id) return std::unexpected{pose_id.error()};
                asset_.icon_pose_id = *pose_id;
                break;
            }
            case tag_palette: {
                auto palette = read_palette(reader_);
                if (!palette) return std::unexpected{palette.error()};
                global_palette_ = std::move(*palette);
                break;
            }
            case tag_copyright:
            case tag_original_url:
            case tag_override_url: {
                const auto maximum = *tag == tag_copyright ? 256U : 512U;
                auto value = reader_.string(maximum);
                if (!value) return std::unexpected{AvatarAssetError::invalid_record};
                if (*tag == tag_copyright) asset_.copyright = std::move(*value);
                else if (*tag == tag_original_url) asset_.original_url = std::move(*value);
                else asset_.override_url = std::move(*value);
                break;
            }
            case tag_usage_flags:
                if (!reader_.skip(1)) return std::unexpected{AvatarAssetError::truncated};
                break;
            case tag_offset_adjustment: {
                const auto value = reader_.i32();
                if (!value) return std::unexpected{AvatarAssetError::truncated};
                const auto adjusted = resource_adjustment_ + static_cast<std::int64_t>(*value);
                if (adjusted < std::numeric_limits<std::int32_t>::min() ||
                    adjusted > std::numeric_limits<std::int32_t>::max())
                    return std::unexpected{AvatarAssetError::invalid_offset};
                resource_adjustment_ = adjusted;
                break;
            }
            case tag_bodies_old:
            case tag_bodies:
                if (auto result = parse_components(asset_.bodies, *tag == tag_bodies_old, ComponentKind::body); !result)
                    return result;
                break;
            case tag_faces_old:
            case tag_faces:
                if (asset_.kind != AvatarKind::complex)
                    return std::unexpected{AvatarAssetError::invalid_record};
                if (auto result = parse_components(asset_.faces, *tag == tag_faces_old, ComponentKind::face); !result)
                    return result;
                break;
            case tag_torsos_old:
            case tag_torsos:
                if (asset_.kind != AvatarKind::complex)
                    return std::unexpected{AvatarAssetError::invalid_record};
                if (auto result = parse_components(asset_.torsos, *tag == tag_torsos_old, ComponentKind::torso); !result)
                    return result;
                break;
            default:
                if (*tag < tag_icon_new) return std::unexpected{AvatarAssetError::invalid_record};
                if (!reader_.skip(*size)) return std::unexpected{AvatarAssetError::truncated};
                continue;
            }
            if (*tag >= tag_icon_new) {
                const auto consumed = reader_.position() - payload_start;
                if (consumed > *size) return std::unexpected{AvatarAssetError::invalid_record};
                if (!reader_.skip(*size - consumed)) return std::unexpected{AvatarAssetError::truncated};
            }
        }
        if (asset_.name.empty() || asset_.icon_pose_id == 0 ||
            (asset_.kind == AvatarKind::simple && asset_.bodies.empty()) ||
            (asset_.kind == AvatarKind::complex && (asset_.faces.empty() || asset_.torsos.empty())))
            return std::unexpected{AvatarAssetError::invalid_record};
        return {};
    }

    enum class ComponentKind { body, face, torso };

    [[nodiscard]] auto parse_components(std::vector<AvatarComponent>& output, const bool old,
        const ComponentKind kind) -> std::expected<void, AvatarAssetError> {
        const auto count = reader_.u16();
        if (!count) return std::unexpected{AvatarAssetError::truncated};
        if (*count == 0 || *count > maximum_components || !output.empty())
            return std::unexpected{AvatarAssetError::invalid_record};
        output.reserve(*count);
        std::uint32_t previous_image_offset{};
        std::uint16_t previous_pose{};
        for (std::uint16_t record_index = 0; record_index < *count; ++record_index) {
            std::array<std::uint32_t, 3> offsets{};
            for (auto& offset : offsets) {
                const auto value = reader_.u32();
                if (!value) return std::unexpected{AvatarAssetError::truncated};
                offset = *value;
            }
            const auto emotion = reader_.u16();
            const auto intensity = reader_.u8();
            if (!emotion || !intensity) return std::unexpected{AvatarAssetError::truncated};
            AvatarComponent component;
            component.emotion_index = *emotion;
            component.intensity = *intensity;
            auto read_coordinate = [this]() -> std::expected<std::int16_t, AvatarAssetError> {
                const auto value = reader_.i16();
                if (!value) return std::unexpected{AvatarAssetError::truncated};
                return *value;
            };
            if (kind == ComponentKind::body) {
                auto x = read_coordinate();
                auto y = read_coordinate();
                if (!x || !y) return std::unexpected{AvatarAssetError::truncated};
                component.face_x = *x;
                component.face_y = *y;
            } else if (kind == ComponentKind::face) {
                auto cx = read_coordinate();
                auto cy = read_coordinate();
                auto dx = read_coordinate();
                auto dy = read_coordinate();
                auto x = read_coordinate();
                auto y = read_coordinate();
                if (!cx || !cy || !dx || !dy || !x || !y)
                    return std::unexpected{AvatarAssetError::truncated};
                component.center_x = *cx;
                component.center_y = *cy;
                component.center_delta_x = *dx;
                component.center_delta_y = *dy;
                component.face_x = *x;
                component.face_y = *y;
            } else {
                auto cx = read_coordinate();
                auto cy = read_coordinate();
                if (!cx || !cy) return std::unexpected{AvatarAssetError::truncated};
                component.center_x = *cx;
                component.center_y = *cy;
            }

            std::array<std::uint8_t, 3> formats{};
            std::array<std::uint8_t, 3> palettes{};
            if (old) {
                if (!reader_.skip(16)) return std::unexpected{AvatarAssetError::truncated};
            } else {
                for (auto& format : formats) {
                    const auto value = reader_.u8();
                    if (!value) return std::unexpected{AvatarAssetError::truncated};
                    format = *value;
                }
                for (auto& palette : palettes) {
                    const auto value = reader_.u8();
                    if (!value) return std::unexpected{AvatarAssetError::truncated};
                    palette = *value;
                }
            }

            if (offsets[0] != previous_image_offset) {
                auto pose = add_pose(offsets, formats, palettes);
                if (!pose) return std::unexpected{pose.error()};
                previous_pose = *pose;
                previous_image_offset = offsets[0];
            } else if (record_index == 0 || previous_pose == 0) {
                return std::unexpected{AvatarAssetError::invalid_record};
            }
            component.pose_id = previous_pose;
            output.push_back(component);
        }
        return {};
    }

    [[nodiscard]] auto decode_image(const ImageRef& ref) const -> std::expected<DecodedBitmap, AvatarAssetError> {
        if (ref.offset == 0 || ref.offset >= bytes_.size()) return std::unexpected{AvatarAssetError::invalid_offset};
        Reader image_reader{bytes_, ref.offset};
        if (ref.format == ImageFormat::zlib)
            return decode_zlib_image(image_reader, ref.palette, global_palette_);
        if (ref.palette != PaletteType::none) return std::unexpected{AvatarAssetError::invalid_palette};
        return decode_dib_image(image_reader);
    }

    [[nodiscard]] static auto mono_bitmap(const DecodedBitmap& source, const auto& predicate) -> AvatarBitmap {
        AvatarBitmap result;
        result.width = source.bitmap.width;
        result.height = source.bitmap.height;
        result.pixels.reserve(source.indices.size());
        for (const auto index : source.indices)
            result.pixels.push_back(predicate(index) ? color(0, 0, 0) : color(255, 255, 255));
        return result;
    }

    [[nodiscard]] auto decode_poses() -> std::expected<void, AvatarAssetError> {
        asset_.poses.reserve(pose_refs_.size());
        for (const auto& ref : pose_refs_) {
            AvatarPose pose;
            std::array<std::optional<DecodedBitmap>, 3> decoded;
            for (std::size_t index = 0; index < ref.images.size(); ++index) {
                if (ref.images[index].offset == 0) continue;
                auto image = decode_image(ref.images[index]);
                if (!image) return std::unexpected{image.error()};
                decoded[index] = std::move(*image);
            }
            if (!decoded[0]) return std::unexpected{AvatarAssetError::invalid_record};
            if (ref.images[0].palette == PaletteType::masked_mono) {
                if (decoded[0]->bits_per_pixel != 2 || decoded[0]->indices.empty())
                    return std::unexpected{AvatarAssetError::invalid_bitmap};
                pose.drawing = mono_bitmap(*decoded[0], [](const auto value) { return value == 3; });
                pose.mask = mono_bitmap(*decoded[0], [](const auto value) { return (value & 2U) != 0; });
                pose.aura = mono_bitmap(*decoded[0], [](const auto value) { return value != 0; });
            } else {
                pose.drawing = std::move(decoded[0]->bitmap);
                if (decoded[1] && ref.images[1].palette == PaletteType::dual_mask) {
                    if (decoded[1]->bits_per_pixel != 2 || decoded[1]->indices.empty())
                        return std::unexpected{AvatarAssetError::invalid_bitmap};
                    pose.mask = mono_bitmap(*decoded[1], [](const auto value) { return (value & 1U) != 0; });
                    pose.aura = mono_bitmap(*decoded[1], [](const auto value) { return (value & 2U) != 0; });
                } else {
                    if (decoded[1]) pose.mask = std::move(decoded[1]->bitmap);
                    if (decoded[2]) pose.aura = std::move(decoded[2]->bitmap);
                }
            }
            asset_.poses.push_back(std::move(pose));
        }
        return {};
    }

    [[nodiscard]] auto parse_backdrop() -> std::expected<AvatarAsset, AvatarAssetError> {
        for (;;) {
            const auto tag = reader_.u16();
            if (!tag) return std::unexpected{AvatarAssetError::truncated};
            if (*tag == tag_start_data) return std::unexpected{AvatarAssetError::invalid_record};
            if (*tag < tag_icon_new) return std::unexpected{AvatarAssetError::invalid_record};
            const auto size = reader_.u16();
            if (!size || *size > reader_.remaining()) return std::unexpected{AvatarAssetError::truncated};
            const auto payload_start = reader_.position();
            if (*tag == tag_copyright || *tag == tag_original_url || *tag == tag_override_url) {
                auto value = reader_.string(*tag == tag_copyright ? 256 : 512);
                if (!value) return std::unexpected{AvatarAssetError::invalid_record};
                if (*tag == tag_copyright) asset_.copyright = std::move(*value);
                else if (*tag == tag_original_url) asset_.original_url = std::move(*value);
                else asset_.override_url = std::move(*value);
            } else if (*tag == tag_offset_adjustment) {
                const auto value = reader_.i32();
                if (!value) return std::unexpected{AvatarAssetError::truncated};
                const auto adjusted = resource_adjustment_ + static_cast<std::int64_t>(*value);
                if (adjusted < std::numeric_limits<std::int32_t>::min() ||
                    adjusted > std::numeric_limits<std::int32_t>::max())
                    return std::unexpected{AvatarAssetError::invalid_offset};
                resource_adjustment_ = adjusted;
            } else if (*tag == tag_backdrop) {
                const auto raw_offset = reader_.u32();
                const auto format = reader_.u8();
                const auto palette = reader_.u8();
                if (!raw_offset || !format || !palette ||
                    (*palette != static_cast<std::uint8_t>(PaletteType::local) &&
                     *palette != static_cast<std::uint8_t>(PaletteType::none)))
                    return std::unexpected{AvatarAssetError::invalid_record};
                const auto ref = image_ref(*raw_offset, *format, *palette);
                if (!ref) return std::unexpected{AvatarAssetError::invalid_offset};
                auto decoded = decode_image(*ref);
                if (!decoded) return std::unexpected{decoded.error()};
                asset_.backdrop = std::move(decoded->bitmap);
                return std::move(asset_);
            }
            const auto consumed = reader_.position() - payload_start;
            if (consumed > *size || !reader_.skip(*size - consumed))
                return std::unexpected{AvatarAssetError::invalid_record};
        }
    }

    std::span<const std::byte> bytes_;
    Reader reader_;
    AvatarAsset asset_;
    std::vector<std::uint32_t> global_palette_;
    std::vector<PoseRef> pose_refs_;
    std::int64_t resource_adjustment_{};
};

} // namespace

auto load_avatar_asset(const std::filesystem::path& path) -> std::expected<AvatarAsset, AvatarAssetError> {
    try {
        std::ifstream file{path, std::ios::binary | std::ios::ate};
        if (!file) return std::unexpected{AvatarAssetError::io};
        const auto end = file.tellg();
        if (end <= 0 || static_cast<std::uint64_t>(end) > maximum_file_bytes)
            return std::unexpected{AvatarAssetError::invalid_header};
        std::vector<std::byte> bytes(static_cast<std::size_t>(end));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            return std::unexpected{AvatarAssetError::io};
        return AssetParser{bytes}.parse();
    } catch (const std::bad_alloc&) {
        return std::unexpected{AvatarAssetError::allocation};
    }
}

namespace {

constexpr auto legacy_pi = 3.14159265358979323846;

[[nodiscard]] auto legacy_emotion(const std::uint16_t index) noexcept -> double {
    if (index >= 1 && index <= 8) return static_cast<double>(index - 1U) * 2.0 * legacy_pi / 8.0;
    if (index == 9 || index == 0) return 0.0;
    if (index >= 10 && index <= 17) return 1001.0 + static_cast<double>(index - 10U);
    return 0.0;
}

[[nodiscard]] auto subtract_angles(const double left, const double right) noexcept -> double {
    const auto difference = left - right;
    if (difference > legacy_pi) return difference - 2.0 * legacy_pi;
    if (difference <= -legacy_pi) return difference + 2.0 * legacy_pi;
    return difference;
}

[[nodiscard]] auto select_rotating_component(const std::span<const AvatarComponent> components,
    const AvatarExpression expression, const std::optional<std::size_t> previous) -> std::optional<std::size_t> {
    double nearest_intensity = 2.0;
    std::optional<std::size_t> nearest;
    for (std::size_t offset = 0; offset < components.size(); ++offset) {
        const auto index = previous ? (*previous + 1U + offset) % components.size() : offset;
        const auto component_emotion = legacy_emotion(components[index].emotion_index);
        if (component_emotion > 7.0) continue;
        const auto angle = std::abs(subtract_angles(component_emotion, expression.angle));
        const auto neutral = component_emotion == 0.0 && components[index].intensity == 0 && !nearest;
        if (angle < legacy_pi / 8.0 || neutral) {
            const auto component_intensity = static_cast<double>(components[index].intensity) / 255.0;
            const auto delta = neutral && expression.intensity > 0.0
                ? 1.5 : std::abs(expression.intensity - component_intensity);
            if (delta < nearest_intensity) {
                nearest_intensity = delta;
                nearest = index;
            }
        }
    }
    return nearest;
}

[[nodiscard]] auto select_face(const std::span<const AvatarComponent> faces, const AvatarExpression expression)
    -> std::optional<std::size_t> {
    auto nearest_angle = 3.0 * legacy_pi;
    auto nearest_intensity = 2.0;
    std::optional<std::size_t> nearest;
    for (std::size_t index = 0; index < faces.size(); ++index) {
        const auto angle = std::abs(subtract_angles(legacy_emotion(faces[index].emotion_index), expression.angle));
        if (angle <= nearest_angle) {
            const auto intensity = static_cast<double>(faces[index].intensity) / 255.0;
            const auto delta = std::abs(expression.intensity - intensity);
            if (angle == nearest_angle && delta >= nearest_intensity) continue;
            nearest_angle = angle;
            nearest_intensity = delta;
            nearest = index;
        }
    }
    return nearest;
}

struct Rect final {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};
};

[[nodiscard]] auto legacy_round(const double value) noexcept -> std::int32_t {
    return static_cast<std::int32_t>(value > 0.0 ? value + 0.5 : value - 0.5);
}

[[nodiscard]] auto channel(const std::uint32_t pixel, const unsigned shift) noexcept -> std::uint8_t {
    return static_cast<std::uint8_t>((pixel >> shift) & 0xffU);
}

[[nodiscard]] auto lerp_channel(const std::uint32_t a, const std::uint32_t b, const std::uint32_t c,
    const std::uint32_t d, const unsigned shift, const double tx, const double ty) noexcept -> std::uint8_t {
    const auto top = static_cast<double>(channel(a, shift)) * (1.0 - tx) + static_cast<double>(channel(b, shift)) * tx;
    const auto bottom = static_cast<double>(channel(c, shift)) * (1.0 - tx) + static_cast<double>(channel(d, shift)) * tx;
    return static_cast<std::uint8_t>(std::clamp(std::lround(top * (1.0 - ty) + bottom * ty), 0L, 255L));
}

[[nodiscard]] auto sample_bilinear(const AvatarBitmap& source, const double x, const double y) noexcept
    -> std::uint32_t {
    const auto x0 = std::clamp(static_cast<std::int32_t>(std::floor(x)), 0, source.width - 1);
    const auto y0 = std::clamp(static_cast<std::int32_t>(std::floor(y)), 0, source.height - 1);
    const auto x1 = std::min(x0 + 1, source.width - 1);
    const auto y1 = std::min(y0 + 1, source.height - 1);
    const auto tx = std::clamp(x - std::floor(x), 0.0, 1.0);
    const auto ty = std::clamp(y - std::floor(y), 0.0, 1.0);
    const auto at = [&source](const std::int32_t px, const std::int32_t py) {
        return source.pixels[static_cast<std::size_t>(py) * static_cast<std::size_t>(source.width) +
            static_cast<std::size_t>(px)];
    };
    const auto a = at(x0, y0);
    const auto b = at(x1, y0);
    const auto c = at(x0, y1);
    const auto d = at(x1, y1);
    return color(lerp_channel(a, b, c, d, 16, tx, ty), lerp_channel(a, b, c, d, 8, tx, ty),
        lerp_channel(a, b, c, d, 0, tx, ty));
}

[[nodiscard]] auto pixel_at(const AvatarBitmap& source, const std::int32_t x, const std::int32_t y) noexcept
    -> std::uint32_t {
    const auto clamped_x = std::clamp(x, 0, source.width - 1);
    const auto clamped_y = std::clamp(y, 0, source.height - 1);
    return source.pixels[static_cast<std::size_t>(clamped_y) * static_cast<std::size_t>(source.width) +
        static_cast<std::size_t>(clamped_x)];
}

[[nodiscard]] auto pixel_luma(const std::uint32_t pixel) noexcept -> double {
    return (static_cast<double>(channel(pixel, 16)) * 0.2126 +
        static_cast<double>(channel(pixel, 8)) * 0.7152 +
        static_cast<double>(channel(pixel, 0)) * 0.0722) / 255.0;
}

struct EdgeDirection final {
    float normal_x{1.0F};
    float normal_y{};
    float strength{};
};

[[nodiscard]] auto make_edge_field(const AvatarBitmap& source) -> std::vector<EdgeDirection> {
    std::vector<EdgeDirection> result(static_cast<std::size_t>(source.width) *
        static_cast<std::size_t>(source.height));
    const auto luma = [&source](const std::int32_t px, const std::int32_t py) {
        return pixel_luma(pixel_at(source, px, py));
    };
    for (std::int32_t y = 0; y < source.height; ++y) {
        for (std::int32_t x = 0; x < source.width; ++x) {
            const auto gradient_x =
                -luma(x - 1, y - 1) + luma(x + 1, y - 1) - 2.0 * luma(x - 1, y) +
                2.0 * luma(x + 1, y) - luma(x - 1, y + 1) + luma(x + 1, y + 1);
            const auto gradient_y =
                -luma(x - 1, y - 1) - 2.0 * luma(x, y - 1) - luma(x + 1, y - 1) +
                luma(x - 1, y + 1) + 2.0 * luma(x, y + 1) + luma(x + 1, y + 1);
            const auto gradient = std::hypot(gradient_x, gradient_y);
            auto& direction = result[static_cast<std::size_t>(y) * static_cast<std::size_t>(source.width) +
                static_cast<std::size_t>(x)];
            direction.strength = static_cast<float>(std::clamp(gradient / 2.0, 0.0, 1.0));
            if (gradient > 1.0e-8) {
                direction.normal_x = static_cast<float>(gradient_x / gradient);
                direction.normal_y = static_cast<float>(gradient_y / gradient);
            }
        }
    }
    return result;
}

// Reconstruct the source as a continuous, coverage-correct image. A local Sobel
// gradient turns the Gaussian footprint so it is narrow across an ink edge and
// wider along it. That smooths staircase contours without the colour wash and
// landmark drift of a conventional isotropic bilinear upscale.
[[nodiscard]] auto sample_edge_directed(const AvatarBitmap& source, const std::span<const EdgeDirection> edges,
    const double x, const double y, const double footprint_x, const double footprint_y) noexcept -> std::uint32_t {
    const auto anchor_x = static_cast<std::int32_t>(std::floor(x + 0.5));
    const auto anchor_y = static_cast<std::int32_t>(std::floor(y + 0.5));
    const auto direction_x = std::clamp(anchor_x, 0, source.width - 1);
    const auto direction_y = std::clamp(anchor_y, 0, source.height - 1);
    const auto& direction = edges[static_cast<std::size_t>(direction_y) * static_cast<std::size_t>(source.width) +
        static_cast<std::size_t>(direction_x)];
    const auto edge_strength = static_cast<double>(direction.strength);
    const auto normal_x = static_cast<double>(direction.normal_x);
    const auto normal_y = static_cast<double>(direction.normal_y);
    const auto tangent_x = -normal_y;
    const auto tangent_y = normal_x;

    // The footprint expands for a downscale, but never becomes a boxy nearest
    // neighbour footprint during the intended 2x/4x remaster path.
    const auto input_footprint = std::max(footprint_x, footprint_y);
    const auto flat_sigma = std::max(0.52, input_footprint * 0.52);
    const auto normal_sigma = std::max(0.38, input_footprint * 0.45);
    const auto tangent_sigma = std::max(0.76, input_footprint * 0.55);
    const auto sigma_normal = flat_sigma * (1.0 - edge_strength) + normal_sigma * edge_strength;
    const auto sigma_tangent = flat_sigma * (1.0 - edge_strength) + tangent_sigma * edge_strength;

    double weights{};
    std::array<double, 3> accumulated{};
    const auto radius = input_footprint > 1.0 ? 3 : 2;
    for (std::int32_t py = anchor_y - radius; py <= anchor_y + radius; ++py) {
        for (std::int32_t px = anchor_x - radius; px <= anchor_x + radius; ++px) {
            const auto delta_x = static_cast<double>(px) - x;
            const auto delta_y = static_cast<double>(py) - y;
            const auto across = delta_x * normal_x + delta_y * normal_y;
            const auto along = delta_x * tangent_x + delta_y * tangent_y;
            const auto exponent = -0.5 * (across * across / (sigma_normal * sigma_normal) +
                along * along / (sigma_tangent * sigma_tangent));
            if (exponent < -12.0) continue;
            const auto weight = std::exp(exponent);
            const auto pixel = pixel_at(source, px, py);
            // AVB colour channels are paint coverage, not emitted-light
            // samples. Interpolating their encoded values preserves the dark
            // ink weight; linear-light interpolation visibly washes it out.
            accumulated[0] += static_cast<double>(channel(pixel, 16)) * weight;
            accumulated[1] += static_cast<double>(channel(pixel, 8)) * weight;
            accumulated[2] += static_cast<double>(channel(pixel, 0)) * weight;
            weights += weight;
        }
    }
    if (weights <= 0.0) return pixel_at(source, anchor_x, anchor_y);
    const auto encode = [weights](const double value) {
        return static_cast<std::uint8_t>(std::clamp(std::lround(value / weights), 0L, 255L));
    };
    return color(encode(accumulated[0]), encode(accumulated[1]), encode(accumulated[2]));
}

enum class RasterOperation { merge_paint, source_and };

void paint_scaled(AvatarBitmap& destination, const AvatarBitmap& source, const Rect rect,
    const bool flip, const RasterOperation operation, const AvatarRenderMode mode) {
    const auto rectangle_width = std::abs(rect.right - rect.left);
    const auto rectangle_height = std::abs(rect.bottom - rect.top);
    if (rectangle_width == 0 || rectangle_height == 0 || source.width <= 0 || source.height <= 0) return;
    const auto start_x = std::min(rect.left, rect.right);
    const auto start_y = std::min(rect.top, rect.bottom);
    const auto edge_field = mode == AvatarRenderMode::modern_remaster
        ? make_edge_field(source) : std::vector<EdgeDirection>{};
    for (std::int32_t output_y = 0; output_y < rectangle_height; ++output_y) {
        const auto destination_y = start_y + output_y;
        if (destination_y < 0 || destination_y >= destination.height) continue;
        const auto source_y = static_cast<double>(output_y) * source.height / rectangle_height;
        for (std::int32_t output_x = 0; output_x < rectangle_width; ++output_x) {
            const auto destination_x = start_x + output_x;
            if (destination_x < 0 || destination_x >= destination.width) continue;
            const auto logical_x = flip ? rectangle_width - output_x - 1 : output_x;
            const auto source_x = mode == AvatarRenderMode::legacy_exact
                ? static_cast<double>(logical_x) * source.width / rectangle_width
                : (static_cast<double>(logical_x) + 0.5) * source.width / rectangle_width - 0.5;
            const auto source_pixel = mode == AvatarRenderMode::legacy_exact
                ? sample_bilinear(source, source_x, source_y)
                : sample_edge_directed(source, edge_field, source_x,
                    (static_cast<double>(output_y) + 0.5) * source.height / rectangle_height - 0.5,
                    static_cast<double>(source.width) / rectangle_width,
                    static_cast<double>(source.height) / rectangle_height);
            auto& destination_pixel = destination.pixels[
                static_cast<std::size_t>(destination_y) * static_cast<std::size_t>(destination.width) +
                static_cast<std::size_t>(destination_x)];
            // Win32 MERGEPAINT is the ternary raster operation D | ~S, not
            // SRCPAINT (D | S). This distinction is visible exactly where a
            // head mask cuts a torso or a torso mask cuts the head.
            const auto rgb = operation == RasterOperation::merge_paint
                ? ((destination_pixel | ~source_pixel) & 0x00ffffffU)
                : ((destination_pixel & source_pixel) & 0x00ffffffU);
            destination_pixel = 0xff000000U | rgb;
        }
    }
}

[[nodiscard]] auto checked_pose(const AvatarAsset& asset, const AvatarComponent& component)
    -> std::expected<const AvatarPose*, AvatarAssetError> {
    if (component.pose_id == 0 || component.pose_id > asset.poses.size())
        return std::unexpected{AvatarAssetError::invalid_record};
    const auto& pose = asset.poses[component.pose_id - 1U];
    if (!pose.drawing) return std::unexpected{AvatarAssetError::invalid_record};
    return &pose;
}

} // namespace

auto select_avatar_expression(const AvatarAsset& asset, const AvatarExpression expression,
    const std::optional<AvatarSelection> previous)
    -> std::expected<AvatarSelection, AvatarAssetError> {
    if (asset.kind == AvatarKind::simple) {
        const auto body = select_rotating_component(asset.bodies, expression,
            previous ? std::optional<std::size_t>{previous->body} : std::nullopt);
        if (!body) return std::unexpected{AvatarAssetError::invalid_record};
        return AvatarSelection{*body, 0, 0};
    }
    if (asset.kind == AvatarKind::complex) {
        const auto face = select_face(asset.faces, expression);
        const auto torso = select_rotating_component(asset.torsos, expression,
            previous ? std::optional<std::size_t>{previous->torso} : std::nullopt);
        if (!face || !torso) return std::unexpected{AvatarAssetError::invalid_record};
        return AvatarSelection{0, *face, *torso};
    }
    return std::unexpected{AvatarAssetError::invalid_record};
}

namespace {

// Shared avatar flag bits (avatar.h:184-186): identical values in the AVB
// usage-flags byte and in the source CAvatarX::m_flags.
constexpr auto avatar_flag_head_mask = std::uint8_t{1};
constexpr auto avatar_flag_torso_mask = std::uint8_t{2};
constexpr auto avatar_flag_torso_first = std::uint8_t{4};

[[nodiscard]] auto to_rect(const AvatarRect& rect) noexcept -> Rect {
    return Rect{rect.left, rect.top, rect.right, rect.bottom};
}

// CBodySingle::GetBodyBox (bodycam.cpp:673-696): fit the pose bitmap into the
// client rect preserving aspect, centred horizontally and bottom-aligned.
[[nodiscard]] auto single_body_box(const AvatarBitmap& drawing, const std::int32_t width,
    const std::int32_t height, const bool flip) -> AvatarBodyBox {
    const auto width_scale = static_cast<double>(width) / drawing.width;
    const auto height_scale = static_cast<double>(height) / drawing.height;
    std::int32_t full_width{};
    std::int32_t full_height{};
    if (width_scale <= height_scale) {
        full_width = width;
        full_height = static_cast<std::int32_t>(width_scale * drawing.height);
    } else {
        full_height = height;
        full_width = static_cast<std::int32_t>(height_scale * drawing.width);
    }
    AvatarRect full{(width - full_width) / 2, height - full_height,
        (width - full_width) / 2 + full_width, height};
    // CBodySingle::FlipBodyBox (bodycam.cpp:595-599) swaps left/right so
    // StretchBlt receives a negative width; paint_scaled normalises the extent
    // and mirrors the sampled column, reproducing the same pixels.
    if (flip) std::swap(full.left, full.right);
    return AvatarBodyBox{full, full, full, false};
}

} // namespace

auto avatar_body_box(const AvatarAsset& asset, const AvatarSelection& selection,
    const std::int32_t width, const std::int32_t height, const bool flip)
    -> std::expected<AvatarBodyBox, AvatarAssetError> {
    if (width <= 0 || height <= 0) return std::unexpected{AvatarAssetError::invalid_bitmap};

    if (asset.kind == AvatarKind::simple) {
        if (selection.body >= asset.bodies.size())
            return std::unexpected{AvatarAssetError::invalid_record};
        auto pose_result = checked_pose(asset, asset.bodies[selection.body]);
        if (!pose_result) return std::unexpected{pose_result.error()};
        return single_body_box(*(*pose_result)->drawing, width, height, flip);
    }

    if (asset.kind != AvatarKind::complex || selection.face >= asset.faces.size() ||
        selection.torso >= asset.torsos.size())
        return std::unexpected{AvatarAssetError::invalid_record};
    const auto& face_component = asset.faces[selection.face];
    const auto& torso_component = asset.torsos[selection.torso];
    auto head_result = checked_pose(asset, face_component);
    auto torso_result = checked_pose(asset, torso_component);
    if (!head_result) return std::unexpected{head_result.error()};
    if (!torso_result) return std::unexpected{torso_result.error()};
    const auto& head = *(*head_result)->drawing;
    const auto& torso = *(*torso_result)->drawing;

    // CBodyDouble::GetBodyBox (bodycam.cpp:632-671): the head offset stacks the
    // torso centre, the face's delta and the face's own centre.
    const auto x_offset = static_cast<std::int32_t>(torso_component.center_x) +
        face_component.center_delta_x - face_component.center_x;
    const auto y_offset = static_cast<std::int32_t>(torso_component.center_y) +
        face_component.center_delta_y - face_component.center_y;
    const auto bit_left = std::min(0, x_offset);
    const auto bit_top = std::min(0, y_offset);
    const auto bit_right = std::max(torso.width, x_offset + head.width);
    const auto bit_bottom = std::max(torso.height, y_offset + head.height);
    const auto bit_width = bit_right - bit_left;
    const auto bit_height = bit_bottom - bit_top;
    if (bit_width <= 0 || bit_height <= 0) return std::unexpected{AvatarAssetError::invalid_bitmap};
    const auto scale = std::min(static_cast<double>(width) / bit_width,
        static_cast<double>(height) / bit_height);
    const auto full_width = legacy_round(scale * bit_width);
    const auto full_height = legacy_round(scale * bit_height);
    const AvatarRect full{(width - full_width) / 2, height - full_height,
        (width - full_width) / 2 + full_width, height};
    const auto make_rect = [&](const std::int32_t offset_x, const std::int32_t offset_y,
        const AvatarBitmap& bitmap) {
        const auto left = legacy_round((offset_x - bit_left) * scale) + full.left;
        const auto top = legacy_round((offset_y - bit_top) * scale) + full.top;
        return AvatarRect{left, top, left + legacy_round(bitmap.width * scale) + 1,
            top + legacy_round(bitmap.height * scale) + 1};
    };
    auto head_rect = make_rect(x_offset, y_offset, head);
    auto torso_rect = make_rect(0, 0, torso);
    if (flip) {
        // CBodyDouble::FlipBodyBox mirrors the component placements around
        // fullRect before StretchBlt receives a negative width. Mirroring
        // pixels in the old rectangles leaves an offset head on the wrong side
        // of an asymmetrical torso.
        const auto flip_rect = [&full](AvatarRect& rect) {
            const auto rect_width = rect.right - rect.left;
            rect.left = full.right - (rect.left - full.left);
            rect.right = rect.left - rect_width;
        };
        flip_rect(head_rect);
        flip_rect(torso_rect);
    }
    return AvatarBodyBox{full, head_rect, torso_rect, true};
}

auto avatar_dim_info(const AvatarAsset& asset, const AvatarSelection& selection, const bool flip)
    -> std::expected<AvatarDimInfo, AvatarAssetError> {
    if (asset.kind == AvatarKind::simple) {
        // CBodySingle::GetDimInfo (avatar.cpp:55-75).
        if (selection.body >= asset.bodies.size())
            return std::unexpected{AvatarAssetError::invalid_record};
        const auto& body = asset.bodies[selection.body];
        auto pose_result = checked_pose(asset, body);
        if (!pose_result) return std::unexpected{pose_result.error()};
        const auto& drawing = *(*pose_result)->drawing;
        const auto width = static_cast<std::int16_t>(drawing.width);
        const auto height = static_cast<std::int16_t>(drawing.height);
        auto face_x = body.face_x;
        if (flip) face_x = static_cast<std::int16_t>(width - face_x);
        return AvatarDimInfo{width, height, 100,
            static_cast<std::int16_t>(height / 2), face_x};
    }

    // CBodyDouble::GetDimInfo (avatar.cpp:77-114): the same union box as
    // GetBodyBox, but head_height carries the composed face bottom.
    if (asset.kind != AvatarKind::complex || selection.face >= asset.faces.size() ||
        selection.torso >= asset.torsos.size())
        return std::unexpected{AvatarAssetError::invalid_record};
    const auto& face_component = asset.faces[selection.face];
    const auto& torso_component = asset.torsos[selection.torso];
    auto head_result = checked_pose(asset, face_component);
    auto torso_result = checked_pose(asset, torso_component);
    if (!head_result) return std::unexpected{head_result.error()};
    if (!torso_result) return std::unexpected{torso_result.error()};
    const auto& head = *(*head_result)->drawing;
    const auto& torso = *(*torso_result)->drawing;
    const auto x_offset = static_cast<std::int32_t>(torso_component.center_x) +
        face_component.center_delta_x - face_component.center_x;
    const auto y_offset = static_cast<std::int32_t>(torso_component.center_y) +
        face_component.center_delta_y - face_component.center_y;
    const auto bit_left = std::min(0, x_offset);
    const auto bit_top = std::min(0, y_offset);
    const auto bit_right = std::max(torso.width, x_offset + head.width);
    const auto composed_head_bottom = y_offset + head.height;
    const auto bit_bottom = std::max(torso.height, composed_head_bottom);
    const auto width = static_cast<std::int16_t>(bit_right - bit_left);
    const auto height = static_cast<std::int16_t>(bit_bottom - bit_top);
    const auto head_height = static_cast<std::int16_t>(composed_head_bottom - bit_top);
    auto face_x = static_cast<std::int16_t>(face_component.face_x + x_offset - bit_left);
    if (flip) face_x = static_cast<std::int16_t>(width - face_x);
    return AvatarDimInfo{width, height, 100, head_height, face_x};
}

auto render_avatar(const AvatarAsset& asset, const AvatarRenderRequest& request)
    -> std::expected<AvatarBitmap, AvatarAssetError> {
    try {
        if (request.width <= 0 || request.height <= 0 || request.width > maximum_dimension ||
            request.height > maximum_dimension ||
            static_cast<std::uint64_t>(request.width) * request.height > maximum_image_bytes / sizeof(std::uint32_t))
            return std::unexpected{AvatarAssetError::invalid_bitmap};
        auto box_result = avatar_body_box(asset, request.selection, request.width, request.height, request.flip);
        if (!box_result) return std::unexpected{box_result.error()};
        const auto& box = *box_result;
        AvatarBitmap output{request.width, request.height,
            std::vector<std::uint32_t>(static_cast<std::size_t>(request.width) *
                static_cast<std::size_t>(request.height), 0xffffffffU)};

        if (!box.composite) {
            const auto& pose = **checked_pose(asset, asset.bodies[request.selection.body]);
            const auto full = to_rect(box.full);
            if (request.draw_nimbus && pose.aura)
                paint_scaled(output, *pose.aura, full, request.flip, RasterOperation::merge_paint, request.mode);
            paint_scaled(output, *pose.drawing, full, request.flip, RasterOperation::source_and, request.mode);
            return output;
        }

        const auto& head = **checked_pose(asset, asset.faces[request.selection.face]);
        const auto& torso = **checked_pose(asset, asset.torsos[request.selection.torso]);
        const auto head_rect = to_rect(box.head);
        const auto torso_rect = to_rect(box.torso);
        if (request.draw_nimbus) {
            if (torso.aura)
                paint_scaled(output, *torso.aura, torso_rect, request.flip, RasterOperation::merge_paint, request.mode);
            if (head.aura)
                paint_scaled(output, *head.aura, head_rect, request.flip, RasterOperation::merge_paint, request.mode);
        }
        const auto paint_part = [&](const AvatarPose& pose, const Rect rect, const bool use_mask) {
            if (use_mask && pose.mask)
                paint_scaled(output, *pose.mask, rect, request.flip, RasterOperation::merge_paint, request.mode);
            paint_scaled(output, *pose.drawing, rect, request.flip, RasterOperation::source_and, request.mode);
        };
        if ((asset.flags & avatar_flag_torso_first) != 0)
            paint_part(torso, torso_rect, (asset.flags & avatar_flag_torso_mask) != 0);
        paint_part(head, head_rect, (asset.flags & avatar_flag_head_mask) != 0);
        if ((asset.flags & avatar_flag_torso_first) == 0)
            paint_part(torso, torso_rect, (asset.flags & avatar_flag_torso_mask) != 0);
        return output;
    } catch (const std::bad_alloc&) {
        return std::unexpected{AvatarAssetError::allocation};
    }
}

auto write_avatar_png(const AvatarBitmap& bitmap, const std::filesystem::path& path) -> bool {
    if (bitmap.width <= 0 || bitmap.height <= 0 ||
        bitmap.pixels.size() != static_cast<std::size_t>(bitmap.width) * static_cast<std::size_t>(bitmap.height))
        return false;
    const auto stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, bitmap.width);
    if (stride != bitmap.width * static_cast<std::int32_t>(sizeof(std::uint32_t))) return false;
    auto* surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(const_cast<std::uint32_t*>(bitmap.pixels.data())),
        CAIRO_FORMAT_ARGB32, bitmap.width, bitmap.height, stride);
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface) cairo_surface_destroy(surface);
        return false;
    }
    const auto native_path = path.string();
    const auto status = cairo_surface_write_to_png(surface, native_path.c_str());
    cairo_surface_destroy(surface);
    return status == CAIRO_STATUS_SUCCESS;
}

} // namespace comicchat
