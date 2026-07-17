#pragma once

#include "comicchat/cpp26.hpp"
#include "comicchat/layout.hpp"

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

    // The shared logical-coordinate drawing pass foundation (render-port-spec.md
    // §0). Compute the device transform that fits a `source_units`-twip square
    // panel into this canvas — the same mapping render_title_panel uses — so the
    // dependent Phase 2 tracks (balloon geometry, body compositing, expert
    // placement) can emit primitives in twips/Y-up logical space.
    [[nodiscard]] auto panel_transform(double source_units = logical_panel_width) const -> PanelTransform;

    // Fill a logical Rect (twips, Y-up: bottom < top, interior y <= 0) on the
    // device canvas through the panel transform, applying the single Cairo
    // translate(origin)·scale(scale, -scale) device matrix. This is the concrete
    // logical -> device drawing primitive the balloon/body passes build on.
    void fill_logical_rect(const Rect& logical, Rgba color, double source_units = logical_panel_width);

    [[nodiscard]] auto write_png(std::string_view path) const -> bool;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace comicchat
