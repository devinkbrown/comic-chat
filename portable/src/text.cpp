#include "comicchat/text.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <mutex>
#include <utility>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>
#include <unicode/normalizer2.h>
#include <unicode/ustring.h>
#include <unicode/unistr.h>

namespace comicchat {

auto normalize_utf8_nfc(const std::string_view input) -> std::expected<std::string, TextError> {
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::unexpected{TextError::invalid_utf8};
    }
    UErrorCode status = U_ZERO_ERROR;
    std::int32_t utf16_size{};
    u_strFromUTF8(nullptr, 0, &utf16_size, input.data(), static_cast<std::int32_t>(input.size()), &status);
    if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
        return std::unexpected{TextError::invalid_utf8};
    }
    status = U_ZERO_ERROR;
    icu::UnicodeString source;
    auto* buffer = source.getBuffer(utf16_size);
    u_strFromUTF8(buffer, utf16_size, &utf16_size, input.data(), static_cast<std::int32_t>(input.size()), &status);
    source.releaseBuffer(utf16_size);
    if (U_FAILURE(status)) return std::unexpected{TextError::invalid_utf8};

    const auto* normalizer = icu::Normalizer2::getNFCInstance(status);
    if (U_FAILURE(status) || normalizer == nullptr) return std::unexpected{TextError::invalid_utf8};
    icu::UnicodeString normalized;
    normalizer->normalize(source, normalized, status);
    if (U_FAILURE(status)) return std::unexpected{TextError::invalid_utf8};
    std::string output;
    normalized.toUTF8String(output);
    return output;
}

auto find_portable_comic_font() -> std::expected<std::string, TextError> {
    static constexpr std::string_view candidates[]{
        "/usr/share/fonts/truetype/msttcorefonts/Comic_Sans_MS.ttf",
        "/usr/share/fonts/truetype/msttcorefonts/comic.ttf",
        "/usr/share/fonts/TTF/Comic_Sans_MS.ttf",
        "/usr/local/share/fonts/Comic_Sans_MS.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/local/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/local/share/fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
    };
    for (const auto candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate)) return std::string{candidate};
    }
    return std::unexpected{TextError::font_open};
}

class TextEngine::Impl final {
public:
    ~Impl() {
        if (font != nullptr) hb_font_destroy(font);
        if (face != nullptr) FT_Done_Face(face);
        if (library != nullptr) FT_Done_FreeType(library);
    }

    FT_Library library{};
    FT_Face face{};
    hb_font_t* font{};
    std::mutex mutex;
};

TextEngine::TextEngine(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
TextEngine::~TextEngine() = default;

auto TextEngine::create(const std::string_view font_path)
    -> std::expected<std::unique_ptr<TextEngine>, TextError> {
    auto impl = std::make_unique<Impl>();
    if (FT_Init_FreeType(&impl->library) != 0) return std::unexpected{TextError::font_library};
    const std::string path{font_path};
    if (FT_New_Face(impl->library, path.c_str(), 0, &impl->face) != 0) {
        return std::unexpected{TextError::font_open};
    }
    impl->font = hb_ft_font_create_referenced(impl->face);
    if (impl->font == nullptr) return std::unexpected{TextError::font_open};
    return std::unique_ptr<TextEngine>{new TextEngine{std::move(impl)}};
}

auto TextEngine::shape(const std::string_view utf8, const double pixel_size)
    -> std::expected<std::vector<ShapedGlyph>, TextError> {
    if (!std::isfinite(pixel_size) || pixel_size <= 0.0 ||
        utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected{TextError::font_size};
    }
    const auto normalized = normalize_utf8_nfc(utf8);
    if (!normalized) return std::unexpected{normalized.error()};
    std::scoped_lock lock{impl_->mutex};
    const auto rounded_size = static_cast<FT_UInt>(std::max(1.0, std::round(pixel_size)));
    if (FT_Set_Pixel_Sizes(impl_->face, 0, rounded_size) != 0) return std::unexpected{TextError::font_size};
    hb_ft_font_changed(impl_->font);
    auto* buffer = hb_buffer_create();
    if (buffer == nullptr) return std::unexpected{TextError::shaping};
    hb_buffer_add_utf8(buffer, normalized->data(), static_cast<int>(normalized->size()), 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(impl_->font, buffer, nullptr, 0);
    unsigned int count{};
    const auto* infos = hb_buffer_get_glyph_infos(buffer, &count);
    const auto* positions = hb_buffer_get_glyph_positions(buffer, &count);
    std::vector<ShapedGlyph> result;
    result.reserve(count);
    for (unsigned int index = 0; index < count; ++index) {
        result.push_back({
            infos[index].codepoint,
            static_cast<double>(positions[index].x_advance) / 64.0,
            static_cast<double>(positions[index].y_advance) / 64.0,
            static_cast<double>(positions[index].x_offset) / 64.0,
            static_cast<double>(positions[index].y_offset) / 64.0,
        });
    }
    hb_buffer_destroy(buffer);
    return result;
}

auto TextEngine::native_face() const noexcept -> void* { return impl_->face; }

} // namespace comicchat
