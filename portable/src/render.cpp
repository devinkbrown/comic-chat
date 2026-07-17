#include "comicchat/render.hpp"

#include "comicchat/balloon.hpp"
#include "comicchat/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include <cairo-ft.h>
#include <cairo.h>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace comicchat {
namespace {

constexpr double source_panel_units = 2300.0;
constexpr double source_icon_size = 500.0;
constexpr double source_icon_space = 100.0;
constexpr double source_below_starring = 300.0;
constexpr double source_row_height = 500.0;
constexpr double source_reference_width = 4860.0;
constexpr double source_title_font_height = 576.0;
constexpr double source_shout_font_height = 252.0;

struct ImageLayout final {
    int stride{};
    std::size_t pixels{};
};

auto checked_image_layout(const std::int32_t width, const std::int32_t height) -> ImageLayout {
    if (width <= 0 || height <= 0) throw std::invalid_argument{"canvas dimensions must be positive"};
    const auto stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    if (stride < 0 || stride % static_cast<int>(sizeof(std::uint32_t)) != 0)
        throw std::length_error{"canvas stride is not representable"};
    const auto row_pixels = static_cast<std::size_t>(stride) / sizeof(std::uint32_t);
    const auto rows = static_cast<std::size_t>(height);
    if (row_pixels > std::numeric_limits<std::size_t>::max() / rows)
        throw std::length_error{"canvas pixel storage overflows"};
    return {stride, row_pixels * rows};
}

void set_color(cairo_t* context, const Rgba color) {
    cairo_set_source_rgba(context, color.red, color.green, color.blue, color.alpha);
}

auto unpack_color(const std::uint32_t color, const double alpha) -> Rgba {
    return {
        static_cast<double>((color >> 16U) & 0xffU) / 255.0,
        static_cast<double>((color >> 8U) & 0xffU) / 255.0,
        static_cast<double>(color & 0xffU) / 255.0,
        alpha,
    };
}

auto shape_advance(const std::vector<ShapedGlyph>& glyphs) -> double {
    double advance{};
    for (const auto& glyph : glyphs) advance += glyph.x_advance;
    return advance;
}

auto text_advance(TextEngine& text, const std::string_view value, const double size) -> double {
    const auto shaped = text.shape(value, size);
    return shaped ? shape_advance(*shaped) : 0.0;
}

auto wrap_words(TextEngine& text, const std::string_view value, const double size, const double maximum_width)
    -> std::vector<std::string> {
    std::vector<std::string> lines;
    std::string current;
    std::size_t cursor{};
    while (cursor < value.size()) {
        while (cursor < value.size() && value[cursor] == ' ') ++cursor;
        const auto end = value.find(' ', cursor);
        const auto word = value.substr(cursor, end == std::string_view::npos ? value.size() - cursor : end - cursor);
        if (word.empty()) break;
        auto candidate = current.empty() ? std::string{word} : current + ' ' + std::string{word};
        if (!current.empty() && text_advance(text, candidate, size) > maximum_width) {
            lines.push_back(std::move(current));
            current = std::string{word};
        } else {
            current = std::move(candidate);
        }
        cursor = end == std::string_view::npos ? value.size() : end + 1;
    }
    if (!current.empty()) lines.push_back(std::move(current));
    if (lines.empty()) lines.emplace_back(value);
    return lines;
}

void draw_shaped(cairo_t* context, TextEngine& text, const std::string_view value,
                 const double size, const double x, const double baseline, const bool centered) {
    const auto shaped = text.shape(value, size);
    if (!shaped || shaped->empty()) return;
    auto* face = static_cast<FT_Face>(text.native_face());
    auto* cairo_face = cairo_ft_font_face_create_for_ft_face(face, 0);
    cairo_set_font_face(context, cairo_face);
    cairo_set_font_size(context, size);
    cairo_font_face_destroy(cairo_face);
    std::vector<cairo_glyph_t> glyphs;
    glyphs.reserve(shaped->size());
    double cursor = centered ? x - shape_advance(*shaped) / 2.0 : x;
    double vertical{};
    for (const auto& glyph : *shaped) {
        glyphs.push_back({glyph.index, cursor + glyph.x_offset, baseline - vertical - glyph.y_offset});
        cursor += glyph.x_advance;
        vertical += glyph.y_advance;
    }
    cairo_show_glyphs(context, glyphs.data(), static_cast<int>(glyphs.size()));
}

} // namespace

class Canvas::Impl final {
public:
    Impl(const std::int32_t requested_width, const std::int32_t requested_height)
        : width{requested_width}, height{requested_height}, layout{checked_image_layout(width, height)},
          data(layout.pixels),
          surface{cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(data.data()),
                                                       CAIRO_FORMAT_ARGB32, width, height, layout.stride),
                  cairo_surface_destroy} {
        if (!surface || cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error{"Cairo surface creation failed"};
        context = {cairo_create(surface.get()), cairo_destroy};
        if (!context || cairo_status(context.get()) != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error{"Cairo context creation failed"};
    }

    std::int32_t width{};
    std::int32_t height{};
    ImageLayout layout;
    std::vector<std::uint32_t> data;
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface{nullptr, cairo_surface_destroy};
    std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context{nullptr, cairo_destroy};
};

Canvas::Canvas(const std::int32_t width, const std::int32_t height) : impl_{std::make_unique<Impl>(width, height)} {}
Canvas::~Canvas() = default;
Canvas::Canvas(Canvas&&) noexcept = default;
auto Canvas::operator=(Canvas&&) noexcept -> Canvas& = default;
auto Canvas::width() const noexcept -> std::int32_t { return impl_->width; }
auto Canvas::height() const noexcept -> std::int32_t { return impl_->height; }
auto Canvas::pixels() const noexcept -> std::span<const std::uint32_t> { return impl_->data; }

void Canvas::clear(const Rgba color) {
    cairo_save(impl_->context.get());
    cairo_set_operator(impl_->context.get(), CAIRO_OPERATOR_SOURCE);
    set_color(impl_->context.get(), color);
    cairo_paint(impl_->context.get());
    cairo_restore(impl_->context.get());
    cairo_surface_flush(impl_->surface.get());
}

void Canvas::render_title_panel(const TitlePanel& model, TextEngine& text) {
    auto* context = impl_->context.get();
    // Share the one panel-fitting transform with the logical drawing pass
    // (layout.cpp fit_panel_transform) so the title panel and the balloon/body
    // passes never drift apart.
    const auto transform = fit_panel_transform(impl_->width, impl_->height, source_panel_units);
    const auto scale = transform.scale;
    const auto origin_x = transform.origin_x;
    const auto origin_y = transform.origin_y;
    const auto panel_size = static_cast<double>(std::min(impl_->width, impl_->height));
    cairo_save(context);
    cairo_rectangle(context, origin_x, origin_y, panel_size, panel_size);
    cairo_clip(context);

    const auto title_size = source_title_font_height * source_panel_units / source_reference_width * scale;
    const auto shout_size = source_shout_font_height * source_panel_units / source_reference_width * scale;
    set_color(context, {0.08, 0.07, 0.08, 1.0});
    const auto title_lines = wrap_words(text, model.title, title_size, panel_size * 0.96);
    const auto title_line_height = title_size * 1.12;
    auto title_baseline = origin_y + 100.0 * scale + title_size * 0.84;
    for (const auto& line : title_lines) {
        draw_shaped(context, text, line, title_size, origin_x + panel_size / 2.0, title_baseline, true);
        title_baseline += title_line_height;
    }
    const auto starring_baseline = origin_y + 100.0 * scale + title_lines.size() * title_line_height + shout_size * 0.84;
    draw_shaped(context, text, model.starring, shout_size,
                origin_x + panel_size / 2.0, starring_baseline, true);

    const auto icon_size = source_icon_size * scale;
    const auto row_height = std::max(source_row_height * scale, shout_size * 1.25);
    const auto text_width = std::min(panel_size * 0.50, panel_size - icon_size - source_icon_space * scale);
    const auto content_width = icon_size + source_icon_space * scale + text_width;
    const auto icon_x = origin_x + std::max(0.0, (panel_size - content_width) / 2.0);
    const auto text_x = icon_x + icon_size + source_icon_space * scale;
    auto row_top = starring_baseline + shout_size * 0.24 +
        source_below_starring * source_panel_units / source_reference_width * scale;
    const auto maximum_rows = static_cast<std::size_t>(std::max(0.0, std::floor((origin_y + panel_size - row_top) / row_height)));
    const auto rows = std::min(model.stars.size(), maximum_rows);
    for (std::size_t index = 0; index < rows; ++index) {
        const auto& star = model.stars[index];
        const auto alpha = star.departed ? 0.45 : 1.0;
        const auto center_x = icon_x + icon_size / 2.0;
        const auto center_y = row_top + row_height / 2.0;
        set_color(context, unpack_color(star.color, alpha));
        cairo_arc(context, center_x, center_y, icon_size * 0.47, 0.0, 2.0 * std::acos(-1.0));
        cairo_fill_preserve(context);
        cairo_set_line_width(context, std::max(1.0, 12.0 * scale));
        set_color(context, {0.08, 0.07, 0.08, alpha});
        cairo_stroke(context);
        const auto initial = star.nickname.empty() ? std::string{"?"} : star.nickname.substr(0, 1);
        set_color(context, {1.0, 1.0, 1.0, alpha});
        draw_shaped(context, text, initial, icon_size * 0.52, center_x, center_y + icon_size * 0.18, true);
        set_color(context, {0.08, 0.07, 0.08, alpha});
        draw_shaped(context, text, star.nickname, shout_size, text_x,
                    center_y + shout_size * 0.35, false);
        row_top += row_height;
    }
    cairo_restore(context);
    cairo_surface_flush(impl_->surface.get());
}

namespace {

// Map a panel-local logical (twips, Y-up) point to a device pixel through the
// panel transform, exactly the composition fill_logical_rect applies
// (translate(origin)·scale(scale, -scale)). Doing the mapping by hand — rather
// than via cairo_scale(1,-1) — keeps text glyphs upright while the balloon
// geometry still lands in the flipped logical frame.
auto to_device(const PanelTransform& transform, const BalloonPoint point) -> DevicePoint {
    return transform.to_device(LogicalPoint{static_cast<double>(point.x), static_cast<double>(point.y)});
}

// Trace a closed cubic-bezier outline (beta_closed_bezier output: point 0 then
// (b1,b2,b3) triples) as a device-space Cairo path.
void trace_bezier_outline(cairo_t* context, const PanelTransform& transform,
                          const std::vector<BalloonPoint>& bez) {
    if (bez.size() < 4) return;
    const auto first = to_device(transform, bez.front());
    cairo_move_to(context, first.x, first.y);
    for (std::size_t i = 1; i + 2 < bez.size(); i += 3) {
        const auto c1 = to_device(transform, bez[i]);
        const auto c2 = to_device(transform, bez[i + 1]);
        const auto c3 = to_device(transform, bez[i + 2]);
        cairo_curve_to(context, c1.x, c1.y, c2.x, c2.y, c3.x, c3.y);
    }
    cairo_close_path(context);
}

// Trace a straight-edged polygon (action box corners) as a device path.
void trace_polygon(cairo_t* context, const PanelTransform& transform,
                   const std::vector<BalloonPoint>& pts) {
    if (pts.empty()) return;
    const auto first = to_device(transform, pts.front());
    cairo_move_to(context, first.x, first.y);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const auto p = to_device(transform, pts[i]);
        cairo_line_to(context, p.x, p.y);
    }
    cairo_close_path(context);
}

// The say/whisper tail (balloon.cpp:1538): a filled white pointer with a black
// edge from a gap in the cloud bottom (around the break tip) down to the speaker
// anchor. Not a bit-exact CArc port — the deterministic anchor/xbreak/angle math
// is goldened separately; this is the visual fill.
void draw_tail(cairo_t* context, const PanelTransform& transform, const TailGeometry& tail,
               const double stroke_width, const bool dashed) {
    constexpr int gap = 80;  // BreakSpline gapwidth (balloon.cpp:457)
    const auto left = to_device(transform, BalloonPoint{tail.tip.x - gap, tail.tip.y});
    const auto right = to_device(transform, BalloonPoint{tail.tip.x + gap, tail.tip.y});
    const auto anchor = to_device(transform, tail.anchor);
    cairo_new_path(context);
    cairo_move_to(context, left.x, left.y);
    cairo_line_to(context, anchor.x, anchor.y);
    cairo_line_to(context, right.x, right.y);
    cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
    cairo_fill_preserve(context);
    if (dashed) {
        const double dashes[2] = {stroke_width * 3.0, stroke_width * 2.0};
        cairo_set_dash(context, dashes, 2, 0.0);
    } else {
        cairo_set_dash(context, nullptr, 0, 0.0);
    }
    cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 1.0);
    cairo_set_line_width(context, stroke_width);
    cairo_stroke(context);
    cairo_set_dash(context, nullptr, 0, 0.0);
}

// Blit an Item 2.2 composited avatar raster (AvatarBitmap, 0xAARRGGBB, Y-down
// screen space matching the Win32 RECT the source StretchBlt's into) into the
// device rectangle [dev_x, dev_y]..[dev_x+dev_w, dev_y+dev_h]. The raster byte
// order equals Cairo's ARGB32, so it wraps directly into an image surface; the
// (dev_w/width, dev_h/height) scale reproduces the source StretchBlt of the body
// bitmap into the placed body box. The avatar's own top row lands at the box top
// (dev_y) — the Y-up->Y-down flip already happened when the caller mapped the
// logical body rect corners through the panel transform, so the raster is drawn
// upright without a second flip.
void blit_avatar(cairo_t* context, const AvatarBitmap& bitmap, const double dev_x, const double dev_y,
                 const double dev_w, const double dev_h) {
    if (bitmap.width <= 0 || bitmap.height <= 0 || dev_w <= 0.0 || dev_h <= 0.0) return;
    const auto expected = static_cast<std::size_t>(bitmap.width) * static_cast<std::size_t>(bitmap.height);
    if (bitmap.pixels.size() < expected) return;
    auto* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, bitmap.width, bitmap.height);
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface) cairo_surface_destroy(surface);
        return;
    }
    cairo_surface_flush(surface);
    const auto stride = static_cast<std::size_t>(cairo_image_surface_get_stride(surface));
    auto* destination = cairo_image_surface_get_data(surface);
    const auto row_bytes = static_cast<std::size_t>(bitmap.width) * sizeof(std::uint32_t);
    for (std::int32_t row = 0; row < bitmap.height; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) * stride,
                    bitmap.pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(bitmap.width),
                    row_bytes);
    }
    cairo_surface_mark_dirty(surface);
    cairo_save(context);
    cairo_translate(context, dev_x, dev_y);
    cairo_scale(context, dev_w / static_cast<double>(bitmap.width), dev_h / static_cast<double>(bitmap.height));
    cairo_set_source_surface(context, surface, 0.0, 0.0);
    // Deterministic sampling for the StretchBlt-equivalent scale; nudged onto the
    // pad region so the fitted body box is fully covered.
    cairo_pattern_set_filter(cairo_get_source(context), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(context), CAIRO_EXTEND_PAD);
    cairo_rectangle(context, 0.0, 0.0, static_cast<double>(bitmap.width), static_cast<double>(bitmap.height));
    cairo_fill(context);
    cairo_restore(context);
    cairo_surface_destroy(surface);
}

} // namespace

void Canvas::render_panel(const Panel& panel, TextEngine& text, const PanelAvatarProvider& avatars) {
    auto* context = impl_->context.get();
    const auto transform = fit_panel_transform(impl_->width, impl_->height, source_panel_units);
    const auto stroke_width = std::max(1.0, 28.0 * transform.scale);  // CBWoodringNormal::m_pen 28

    cairo_save(context);

    // Avatar bodies. Map the logical (twips, Y-up) body box through the panel
    // transform to a device rect (this applies the Y-up->Y-down flip once), then
    // blit the Item 2.2 composited raster the provider resolves. When no provider
    // is wired, or it declines a body, fall back to the flat color box so the
    // balloon tails still have something to point at.
    for (const auto& body : panel.bodies) {
        const auto top_left = transform.to_device(LogicalPoint{static_cast<double>(body.box.left),
                                                               static_cast<double>(body.box.top)});
        const auto bottom_right = transform.to_device(LogicalPoint{static_cast<double>(body.box.right),
                                                                   static_cast<double>(body.box.bottom)});
        const auto dev_w = bottom_right.x - top_left.x;
        const auto dev_h = bottom_right.y - top_left.y;

        std::optional<AvatarBitmap> raster;
        if (avatars) {
            raster = avatars(body, static_cast<std::int32_t>(std::lround(dev_w)),
                             static_cast<std::int32_t>(std::lround(dev_h)));
        }
        if (raster && raster->width > 0 && raster->height > 0 &&
            raster->pixels.size() >= static_cast<std::size_t>(raster->width) *
                                         static_cast<std::size_t>(raster->height)) {
            blit_avatar(context, *raster, top_left.x, top_left.y, dev_w, dev_h);
        } else {
            set_color(context, unpack_color(body.color, 1.0));
            cairo_rectangle(context, top_left.x, top_left.y, dev_w, dev_h);
            cairo_fill(context);
        }
    }

    for (const auto& balloon : panel.balloons) {
        // Tail first so the cloud fill overlaps the tail's top edge.
        if (balloon.has_tail) {
            draw_tail(context, transform, balloon.tail, stroke_width, balloon.kind.dashed);
        }

        cairo_new_path(context);
        if (balloon.kind.mode == BalloonMode::action) {
            trace_polygon(context, transform, balloon.outline);
        } else {
            trace_bezier_outline(context, transform, balloon.outline);
        }
        cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
        cairo_fill_preserve(context);
        if (balloon.kind.dashed) {
            const double dashes[2] = {stroke_width * 3.0, stroke_width * 2.0};
            cairo_set_dash(context, dashes, 2, 0.0);
        }
        cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 1.0);
        cairo_set_line_width(context, stroke_width);
        cairo_stroke(context);
        cairo_set_dash(context, nullptr, 0, 0.0);

        // Think trail: shrinking ellipses toward the speaker.
        for (const auto& bubble : balloon.bubbles) {
            const auto center = to_device(transform, bubble.center);
            const auto radius = (static_cast<double>(bubble.radius) + bubble.width_pad) * transform.scale;
            cairo_new_path(context);
            cairo_arc(context, center.x, center.y, std::max(1.0, radius), 0.0, 2.0 * std::acos(-1.0));
            cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
            cairo_fill_preserve(context);
            cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 1.0);
            cairo_set_line_width(context, stroke_width);
            cairo_stroke(context);
        }

        // Balloon text, stacked from the cloud top down (device space, upright).
        const auto text_size = static_cast<double>(balloon.line_height) * transform.scale * 0.72;
        if (text_size >= 1.0 && !balloon.lines.empty()) {
            set_color(context, {0.08, 0.07, 0.08, 1.0});
            const auto center_x = (balloon.bbox.left + balloon.bbox.right) / 2;
            auto baseline_y = balloon.bbox.top - balloon.line_height;
            for (const auto& line : balloon.lines) {
                const auto anchor = transform.to_device(LogicalPoint{static_cast<double>(center_x),
                                                                     static_cast<double>(baseline_y)});
                draw_shaped(context, text, line.text, text_size, anchor.x, anchor.y, true);
                baseline_y -= balloon.line_height;
            }
        }
    }

    cairo_restore(context);
    cairo_surface_flush(impl_->surface.get());
}

auto Canvas::panel_transform(const double source_units) const -> PanelTransform {
    return fit_panel_transform(impl_->width, impl_->height, source_units);
}

void Canvas::fill_logical_rect(const Rect& logical, const Rgba color, const double source_units) {
    auto* context = impl_->context.get();
    const auto transform = fit_panel_transform(impl_->width, impl_->height, source_units);
    cairo_save(context);
    // The single device transform of render-port-spec.md §0: translate to the
    // panel origin, then scale by (scale, -scale) so twips map to pixels and the
    // Y-up logical axis flips to Cairo's Y-down device axis. Drawing below stays
    // entirely in logical (twips, Y-up) coordinates.
    cairo_translate(context, transform.origin_x, transform.origin_y);
    cairo_scale(context, transform.scale, -transform.scale);
    set_color(context, color);
    cairo_rectangle(context, static_cast<double>(logical.left), static_cast<double>(logical.bottom),
                    static_cast<double>(logical.right - logical.left),
                    static_cast<double>(logical.top - logical.bottom));
    cairo_fill(context);
    cairo_restore(context);
    cairo_surface_flush(impl_->surface.get());
}

auto Canvas::write_png(const std::string_view path) const -> bool {
    const std::string owned_path{path};
    return cairo_surface_write_to_png(impl_->surface.get(), owned_path.c_str()) == CAIRO_STATUS_SUCCESS;
}

} // namespace comicchat
