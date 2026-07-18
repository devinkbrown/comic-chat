#pragma once

#include "comicchat/avatar_assets.hpp"
#include "comicchat/balloon.hpp"
#include "comicchat/cpp26.hpp"
#include "comicchat/layout.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace comicchat {

class TextEngine;
class BackdropCatalog;

// Resolve a placed PanelBody to a composited avatar raster (Item 2.2 output:
// render_avatar's AvatarBitmap, 0xAARRGGBB matching Cairo's ARGB32). Given the
// device pixel size render_panel will stretch the raster into, the provider is
// free to composite at that exact resolution for a crisp 1:1 blit, or return a
// natural-size raster and let render_panel StretchBlt-scale it into the body
// rect. Returning std::nullopt (or a default-constructed provider) keeps the
// flat color-box placeholder for that body. Pure and deterministic: the same
// PanelBody + target size must yield the same raster so frames stay
// byte-reproducible. The nick->avatar mapping (Phase 2.5b) is the caller that
// will supply this; here a single default avatar drives it.
using PanelAvatarProvider = std::function<std::optional<AvatarBitmap>(
    const PanelBody& body, std::int32_t target_width, std::int32_t target_height)>;

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

    // Emit an assembled conversation panel (Item 2.1, render-port-spec.md
    // §2.1.f) into the logical drawing pass: an optional scene backdrop (when
    // `panel.backdrop_id` is set and `backdrop_catalog` resolves it), then real
    // composited avatar bodies (when `avatars` resolves a PanelBody to an Item
    // 2.2 raster) or a flat color-box placeholder otherwise, the
    // beta-spline cloud (or action box) outline, the whisper dash / think
    // bubbles / say tail, and the balloon text. Geometry arrives in panel-local
    // twips (Y-up) and is mapped through the single panel transform, exactly as
    // fill_logical_rect. Deterministic: the same Panel yields a byte-identical
    // frame.
    //
    // `backdrop_catalog` mirrors CUnitPanel::Draw's `m_backDrop.Draw(...)` call
    // immediately after the panel clip and before any body draws (panel.cpp:
    // 681,684). It defaults to nullptr so every existing caller keeps compiling
    // and rendering unchanged; when null (or `panel.backdrop_id` is unset), no
    // backdrop pass runs, matching today's behavior exactly. Non-const because
    // BackdropCatalog::resolve_art caches decoded art on first use.
    void render_panel(const Panel& panel, TextEngine& text, const PanelAvatarProvider& avatars = {},
                       BackdropCatalog* backdrop_catalog = nullptr);

    // Compose a full comic STRIP: lay every assembled Panel out on the page grid
    // (layout.cpp panel_rect/page_bounds — a 2-per-row grid with a
    // logical_interstice gutter) and draw each one by wrapping the EXISTING
    // render_panel under a per-cell device transform. The whole page is fit into
    // the canvas centered and aspect-preserving; each panel's 2300-twip square
    // maps into its cell, so the individual panels are never distorted and the
    // gutters keep them from overlapping. A single-panel page reduces to exactly
    // render_panel (the page bounds are one 2300-twip square, the lone cell is the
    // whole page, and the per-cell transform is the identity), so the common live
    // case stays byte-identical. An empty span draws nothing. Deterministic: the
    // same panels yield a byte-identical frame.
    void render_page(std::span<const Panel> panels, TextEngine& text,
                     const PanelAvatarProvider& avatars = {});

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
