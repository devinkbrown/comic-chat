#pragma once

#include "comicchat/cpp26.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace comicchat {

class TextEngine;

struct Rgba final {
    double red{};
    double green{};
    double blue{};
    double alpha{1.0};
};

struct TitleStar final {
    std::string nickname;
    std::uint32_t color{0x6c8ebfU};
    bool departed{};
};

struct TitlePanel final {
    std::string title;
    std::string starring{"STARRING"};
    std::vector<TitleStar> stars;
};

class Canvas final {
public:
    Canvas(std::int32_t width, std::int32_t height);
    ~Canvas();
    Canvas(const Canvas&) = delete;
    auto operator=(const Canvas&) -> Canvas& = delete;
    Canvas(Canvas&&) noexcept;
    auto operator=(Canvas&&) noexcept -> Canvas&;

    [[nodiscard]] auto width() const noexcept -> std::int32_t;
    [[nodiscard]] auto height() const noexcept -> std::int32_t;
    [[nodiscard]] auto pixels() const noexcept -> std::span<const std::uint32_t>;
    void clear(Rgba color);
    void render_title_panel(const TitlePanel& model, TextEngine& text);
    [[nodiscard]] auto write_png(std::string_view path) const -> bool;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace comicchat
