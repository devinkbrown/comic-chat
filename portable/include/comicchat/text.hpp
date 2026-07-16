#pragma once

#include "comicchat/cpp26.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace comicchat {

struct ShapedGlyph final {
    std::uint32_t index{};
    double x_advance{};
    double y_advance{};
    double x_offset{};
    double y_offset{};
};

enum class TextError { invalid_utf8, font_library, font_open, font_size, shaping };

[[nodiscard]] auto normalize_utf8_nfc(std::string_view input) -> std::expected<std::string, TextError>;
[[nodiscard]] auto find_portable_comic_font() -> std::expected<std::string, TextError>;

class TextEngine final {
public:
    static auto create(std::string_view font_path) -> std::expected<std::unique_ptr<TextEngine>, TextError>;
    ~TextEngine();
    TextEngine(const TextEngine&) = delete;
    auto operator=(const TextEngine&) -> TextEngine& = delete;

    [[nodiscard]] auto shape(std::string_view utf8, double pixel_size)
        -> std::expected<std::vector<ShapedGlyph>, TextError>;
    [[nodiscard]] auto native_face() const noexcept -> void*;

private:
    class Impl;
    explicit TextEngine(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace comicchat
