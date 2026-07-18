#include "comicchat/render.hpp"

#include "comicchat/backdrop.hpp"
#include "comicchat/balloon.hpp"
#include "comicchat/formatting.hpp"
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
// CUnitPanel::m_borderWidth (panel.cpp:64). The panel-frame pen is
// PS_SOLID, 2 * m_borderWidth (panel.cpp:65).
constexpr double source_border_width = 60.0;
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

// Default balloon text ink (matches the pre-formatting single-color path,
// render.cpp:518) used when a run carries no explicit foreground override.
constexpr Rgba default_balloon_ink{0.08, 0.07, 0.08, 1.0};
// Link runs render in blue when no explicit fg is set (classic hyperlink /
// mIRC palette index 12), and are always underlined.
constexpr Rgba link_ink{0.0, 0.0, 0.9, 1.0};

// FONT-ASSET DECISION (documented per task): the portable repo bundles only
// assets/fonts/comic.ttf -- there is NO bundled bold or italic Comic Neue face
// (no *_italic*/*_bold* TTF, no COMIC_NEUE_LICENSE.txt for a licensed variant).
// So bold/italic are SYNTHETIC. Crucially we do NOT reach for cairo_select_font_face:
// its toy-font family lookup would DISCARD the bundled Comic Neue FT face and pull
// a non-deterministic system font via fontconfig, breaking the whole point of the
// bundled-font parity pipeline. Instead we keep the real Comic Neue FT face for
// every run and synthesize:
//   * ITALIC  -> a horizontal shear baked into the cairo font matrix (upright
//                glyphs of the SAME face, slanted), advances unchanged.
//   * BOLD    -> faux-bold: draw the glyphs, then hairline-stroke the same glyph
//                path, thickening the strokes (a measurable ink-density increase
//                the gate test asserts on rendered pixels).
// Real fidelity would beat this, but no real face exists to load, so this is the
// honest maximum. Draws one run at [x, baseline); returns the run's x-advance so
// the caller can position the next run. Saves/restores all cairo state it touches
// so nothing (font matrix, face, color, line width) bleeds into the next run,
// line, or balloon.
[[nodiscard]] double draw_run_glyphs(cairo_t* context, TextEngine& text, const std::string_view value,
                                     const double size, const double x, const double baseline,
                                     const Rgba color, const bool bold, const bool italic,
                                     const bool underline) {
    const auto shaped = text.shape(value, size);
    if (!shaped || shaped->empty()) return 0.0;

    cairo_save(context);
    auto* face = static_cast<FT_Face>(text.native_face());
    auto* cairo_face = cairo_ft_font_face_create_for_ft_face(face, 0);
    cairo_set_font_face(context, cairo_face);
    cairo_font_face_destroy(cairo_face);

    // Font matrix = uniform scale, plus a horizontal shear for synthetic italic.
    constexpr double italic_shear = 0.21; // ~12deg slant, typical oblique synthesis.
    cairo_matrix_t font_matrix{};
    cairo_matrix_init(&font_matrix, size, 0.0, italic ? italic_shear * size : 0.0, size, 0.0, 0.0);
    cairo_set_font_matrix(context, &font_matrix);

    std::vector<cairo_glyph_t> glyphs;
    glyphs.reserve(shaped->size());
    double cursor = x;
    double vertical{};
    for (const auto& glyph : *shaped) {
        glyphs.push_back({glyph.index, cursor + glyph.x_offset, baseline - vertical - glyph.y_offset});
        cursor += glyph.x_advance;
        vertical += glyph.y_advance;
    }
    const double advance = cursor - x;

    set_color(context, color);
    cairo_show_glyphs(context, glyphs.data(), static_cast<int>(glyphs.size()));
    if (bold) {
        // Faux-bold: re-trace the glyph outlines and stroke them, thickening
        // every stem. Line width scales with the font size so bold reads at any
        // zoom. This is the ink-density delta the gate test measures.
        cairo_glyph_path(context, glyphs.data(), static_cast<int>(glyphs.size()));
        cairo_set_line_width(context, std::max(0.6, size * 0.045));
        cairo_stroke(context);
    }
    if (underline) {
        // A simple stroked rule just below the baseline spanning the run advance.
        const double thickness = std::max(1.0, size * 0.06);
        const double underline_y = baseline + size * 0.12;
        cairo_new_path(context);
        cairo_move_to(context, x, underline_y);
        cairo_line_to(context, x + advance, underline_y);
        cairo_set_line_width(context, thickness);
        cairo_stroke(context);
    }
    cairo_restore(context);
    return advance;
}

// Resolve a run's cairo ink: explicit fg override wins; else link runs get the
// link color; else the balloon's default ink. bg is intentionally not painted
// (the portable balloon has its own cloud fill; mIRC bg boxes are out of scope).
[[nodiscard]] Rgba run_ink(const TextRun& run) {
    if (run.fg) {
        const auto rgb = palette_color(*run.fg);
        return unpack_color((static_cast<std::uint32_t>(rgb.r) << 16U) |
                                (static_cast<std::uint32_t>(rgb.g) << 8U) | rgb.b,
                            1.0);
    }
    if (run.link) return link_ink;
    return default_balloon_ink;
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

// Trace an OPEN cubic-bezier outline (beta_open_bezier output: point 0 then
// (b1,b2,b3) triples) as a device path WITHOUT closing it. This is the cloud
// broken at the tail throat (BreakSpline, balloon.cpp:477): it runs from the
// right gap edge, over the cloud top, to the left gap edge, and stops there so
// the tail arcs can bridge the gap instead of a stroked-across bottom.
void trace_bezier_outline_open(cairo_t* context, const PanelTransform& transform,
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
}

// One bowed tail edge: DrawArc2 (arc.cpp:97) approximated by a cubic bezier whose
// midpoint sagitta equals `altitude`. A positive altitude bows to the RIGHT of
// the start->end vector in logical (Y-up) space (arc.cpp:95); |altitude| < 1
// degenerates to a straight LineTo (arc.cpp:101-104), which also guards the
// zero-tail (tailLen == 0 -> alt == 0) case with no NaN. The control points are
// built in logical space and mapped through the panel transform, so the Y-up ->
// Y-down handedness of "right of the vector" is preserved. Assumes the current
// path point is already `a` mapped to device.
void trace_bowed_edge(cairo_t* context, const PanelTransform& transform, const BalloonPoint a,
                      const BalloonPoint b, const int altitude) {
    const auto end = to_device(transform, b);
    const double dx = static_cast<double>(b.x - a.x);
    const double dy = static_cast<double>(b.y - a.y);
    const double len = std::hypot(dx, dy);
    if (len < 1e-9 || std::abs(altitude) < 1) {
        cairo_line_to(context, end.x, end.y);
        return;
    }
    // Right normal of the start->end vector in Y-up logical space (arc.cpp:114).
    const double nx = dy / len;
    const double ny = -dx / len;
    // A cubic with both interior handles offset by f along the normal reaches a
    // midpoint deviation of 0.75*f; f = 4/3*altitude makes that deviation exactly
    // the altitude (the circular-arc sagitta), faithful for the shallow 5% bow.
    const double f = (4.0 / 3.0) * static_cast<double>(altitude);
    const LogicalPoint c1_log{static_cast<double>(a.x) + dx / 3.0 + f * nx,
                              static_cast<double>(a.y) + dy / 3.0 + f * ny};
    const LogicalPoint c2_log{static_cast<double>(a.x) + 2.0 * dx / 3.0 + f * nx,
                              static_cast<double>(a.y) + 2.0 * dy / 3.0 + f * ny};
    const auto c1 = transform.to_device(c1_log);
    const auto c2 = transform.to_device(c2_log);
    cairo_curve_to(context, c1.x, c1.y, c2.x, c2.y, end.x, end.y);
}

// The single open cloud + bowed tail figure (CBWoodringNormal::SetBalloonTraj +
// Draw, balloon.cpp:1886-1934). One CTraj = the open cloud spline + two CArc tail
// edges, m_closed = TRUE, StrokeAndFill'd once: trace the open cloud (right gap
// edge -> top -> left gap edge), bow down to the speaker anchor, bow back up to
// the right gap edge (== the move_to start), then close once. The cloud bottom
// between the gap edges is never drawn, so there is no tail-throat seam. Falls
// back to the closed outline if the break degenerated (empty open outline).
void trace_cloud_and_tail(cairo_t* context, const PanelTransform& transform, const Balloon& balloon) {
    const auto& bez = balloon.outline_open;
    if (bez.size() < 4) {
        trace_bezier_outline(context, transform, balloon.outline);
        return;
    }
    trace_bezier_outline_open(context, transform, bez);
    // The open outline ends at the left gap edge; arc down to the anchor, then
    // back up to the right gap edge (bez.front(), the move_to start). The two
    // edges bow with +sign*alt and -sign*alt, curving apart (balloon.cpp:1598-1601).
    const int alt = balloon.tail.altitude;
    const int sign = balloon.tail.tail_sign;
    trace_bowed_edge(context, transform, bez.back(), balloon.tail.anchor, sign * alt);
    trace_bowed_edge(context, transform, balloon.tail.anchor, bez.front(), -sign * alt);
    cairo_close_path(context);
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
//
// `crop`, when non-null, restricts the source region blitted to `crop`'s
// pixel-space [left,right]x[top,bottom] window (BackdropCrop::src's
// convention: origin top-left, rows increasing downward — see backdrop.hpp)
// instead of the whole bitmap, which is how the backdrop draw-hook below
// reuses this same StretchBlt-equivalent pattern for a CBackDrop::Draw-style
// panned/zoomed crop (backdrop.cpp:330-368) rather than a full-bitmap blit.
// nullptr (the default, used by the avatar-body call site) keeps blitting the
// entire bitmap exactly as before.
void blit_avatar(cairo_t* context, const AvatarBitmap& bitmap, const double dev_x, const double dev_y,
                 const double dev_w, const double dev_h, const Rect* crop = nullptr) {
    if (bitmap.width <= 0 || bitmap.height <= 0 || dev_w <= 0.0 || dev_h <= 0.0) return;
    const auto expected = static_cast<std::size_t>(bitmap.width) * static_cast<std::size_t>(bitmap.height);
    if (bitmap.pixels.size() < expected) return;

    const double src_left = crop ? static_cast<double>(crop->left) : 0.0;
    const double src_top = crop ? static_cast<double>(crop->top) : 0.0;
    const double src_w = crop ? static_cast<double>(crop->right - crop->left) : static_cast<double>(bitmap.width);
    const double src_h = crop ? static_cast<double>(crop->bottom - crop->top) : static_cast<double>(bitmap.height);
    if (src_w <= 0.0 || src_h <= 0.0) return;

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
    cairo_scale(context, dev_w / src_w, dev_h / src_h);
    cairo_translate(context, -src_left, -src_top);
    cairo_set_source_surface(context, surface, 0.0, 0.0);
    // Deterministic sampling for the StretchBlt-equivalent scale; nudged onto the
    // pad region so the fitted box is fully covered.
    cairo_pattern_set_filter(cairo_get_source(context), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(context), CAIRO_EXTEND_PAD);
    cairo_rectangle(context, src_left, src_top, src_w, src_h);
    cairo_fill(context);
    cairo_restore(context);
    cairo_surface_destroy(surface);
}

} // namespace

void Canvas::render_panel(const Panel& panel, TextEngine& text, const PanelAvatarProvider& avatars,
                          BackdropCatalog* backdrop_catalog) {
    auto* context = impl_->context.get();
    const auto transform = fit_panel_transform(impl_->width, impl_->height, source_panel_units);
    const auto stroke_width = std::max(1.0, 28.0 * transform.scale);  // CBWoodringNormal::m_pen 28

    cairo_save(context);

    // CUnitPanel::Draw (panel.cpp:666) clips every element to the panel rectangle
    // [0,0]..[m_unitWidth,-m_unitHeight] (IntersectClipRect, panel.cpp:678) before
    // drawing the backdrop, bodies, balloons, and border. Reproduce that clip so a
    // cloud/tail that docks past the panel edge is trimmed exactly as the source,
    // instead of spilling onto the page/letterbox.
    const auto panel_tl = transform.to_device(LogicalPoint{0.0, 0.0});
    const auto panel_br = transform.to_device(LogicalPoint{source_panel_units, -source_panel_units});
    cairo_rectangle(context, panel_tl.x, panel_tl.y, panel_br.x - panel_tl.x, panel_br.y - panel_tl.y);
    cairo_clip(context);

    // Scene backdrop. CUnitPanel::Draw calls m_backDrop.Draw(...) immediately
    // after the clip (panel.cpp:681) and before any body draws (panel.cpp:684),
    // so a configured backdrop always sits behind the avatars/balloons for this
    // panel. Skip entirely (no behavior change from before this field existed)
    // when the panel has no backdrop_id, or when the caller passed no catalog
    // to resolve it against -- this keeps every existing render_panel call site
    // compiling and rendering byte-identically.
    if (panel.backdrop_id && backdrop_catalog) {
        // world_coords == panel_bbox: the degenerate BF_NOZOOM crop window
        // (backdrop.hpp's crop_for_panel docs) -- Panel does not yet carry a
        // per-instance pan/zoom bbox of its own (CBackDrop::m_bbox, set by
        // AdjustArtToCoord, panel.cpp:951-958), so the whole source bitmap is
        // shown stretched to fill the panel, matching CBackDrop's own default
        // (BF_NOZOOM, backdrop.h:36,43) until that per-panel bbox is wired up.
        // `panel_tl`/`panel_br` above are already this same panel_bbox mapped
        // through the device transform, so they double as both the crop
        // window's device rect AND the blit destination rect.
        const Rect panel_world_bbox{.left = 0, .bottom = -static_cast<std::int32_t>(source_panel_units),
                                    .right = static_cast<std::int32_t>(source_panel_units), .top = 0};
        if (auto art = backdrop_catalog->resolve_art(*panel.backdrop_id); art.has_value()) {
            const auto crop = crop_for_panel(art->get(), panel_world_bbox, panel_world_bbox);
            blit_avatar(context, art->get(), panel_tl.x, panel_tl.y, panel_br.x - panel_tl.x,
                       panel_br.y - panel_tl.y, &crop.src);
        }
    }

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

    // CUnitPanel::Draw walks the element list tail->head (panel.cpp:697-704):
    // pePos starts at GetTailPosition() and each GetPrev(pePos) steps toward the
    // head, so the newest-added element (list tail) draws first and the
    // oldest-added element (list head) draws last, on top. page.cpp and
    // comic_page.cpp push_back each balloon in chronological (oldest-first) order
    // -- the same order CUnitPanel::AddBalloon's m_elements.AddTail (panel.cpp:585,
    // 1103) builds the source list in -- so panel.balloons.front() is the source's
    // list head and panel.balloons.back() is the source's list tail. Iterate in
    // reverse to reproduce that draw order for overlapping balloons.
    for (auto it = panel.balloons.rbegin(); it != panel.balloons.rend(); ++it) {
        const auto& balloon = *it;

        // ONE path for the whole balloon: an action box stays a closed rectangle
        // with no tail; every cloud (say/whisper/think) is the single open
        // cloud+tail figure (BreakSpline opened the cloud bottom, the two bowed
        // CArc edges bridge the gap), filled white and stroked once -- no separate
        // tail triangle stroked across the throat, so the seam is gone.
        cairo_new_path(context);
        if (balloon.kind.mode == BalloonMode::action) {
            trace_polygon(context, transform, balloon.outline);
        } else {
            trace_cloud_and_tail(context, transform, balloon);
        }
        cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
        cairo_fill_preserve(context);
        if (balloon.kind.dashed) {
            // CBWoodringNormal::Draw selects m_nimbusPen (PS_SOLID, 100,
            // RGB(255,255,255)) over the filled path whenever m_byteDashed is set
            // (balloon.cpp:114, 1921), THEN re-selects the black m_pen and dashes
            // the trajectory on top (balloon.cpp:1934-1939). Reproduce the white
            // 100-twip nimbus stroke first so it grows the visible cloud outward
            // before the black dashed outline goes on top of it. The source is
            // still white from the fill_preserve above.
            cairo_set_line_width(context, std::max(1.0, 100.0 * transform.scale));
            cairo_stroke_preserve(context);

            // The dashed pen uses a FIXED 100/100-twip on/off pattern
            // (traj.cpp:6, int dashArray[] = {100, 100}) independent of pen width,
            // not the previous pen-relative {3x, 2x} ratio.
            const double dashes[2] = {100.0 * transform.scale, 100.0 * transform.scale};
            cairo_set_dash(context, dashes, 2, 0.0);
        }
        cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 1.0);
        cairo_set_line_width(context, stroke_width);
        cairo_stroke(context);
        cairo_set_dash(context, nullptr, 0, 0.0);

        // Think trail: shrinking ellipses toward the speaker. CBWoodringThink::Draw
        // (balloon.cpp:1997-2001) grows the pad on WIDTH ONLY -- circRect starts as
        // a BUBBLEHEIGHT-square, then `left -= widthAdjustment; right +=
        // widthAdjustment;` before Ellipse(&circRect) -- so each bubble is a
        // width-stretched ellipse, not a circle. Build the ellipse path with a
        // save/translate/scale/arc/restore so the path itself lands in device
        // space pre-distorted; restoring the CTM before the stroke keeps the pen
        // width undistorted (a plain scale+stroke would thin/fatten the outline
        // anisotropically).
        for (const auto& bubble : balloon.bubbles) {
            const auto center = to_device(transform, bubble.center);
            const auto x_radius =
                std::max(1.0, (static_cast<double>(bubble.radius) + bubble.width_pad) * transform.scale);
            const auto y_radius = std::max(1.0, static_cast<double>(bubble.radius) * transform.scale);
            cairo_new_path(context);
            cairo_save(context);
            cairo_translate(context, center.x, center.y);
            cairo_scale(context, x_radius, y_radius);
            cairo_arc(context, 0.0, 0.0, 1.0, 0.0, 2.0 * std::acos(-1.0));
            cairo_restore(context);
            cairo_set_source_rgba(context, 1.0, 1.0, 1.0, 1.0);
            cairo_fill_preserve(context);
            cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 1.0);
            cairo_set_line_width(context, stroke_width);
            cairo_stroke(context);
        }

        // Balloon text (CBalloon::DrawText, balloon.cpp:1315). Faithful port: draw
        // each wrapped line at the SAME font pixel size the cloud was measured and
        // sized at (balloon.text_size, threaded from message_text_size), so the
        // drawn glyphs match the cloud fit and stay inside the outline -- not a
        // 0.72 * line_height fudge. The source stacks lines from the text top:
        // TextOut(rgiLeftX[i], iBaseY) with iBaseY = m_bbox.Top and stepping down
        // by m_lineHeight per line (m_bbox.Top is the TEXT top, one lineHeight
        // above the first baseline in GDI TA_TOP twips). Here bbox.top is that same
        // text top; we place each line's cell top at bbox.top - i*lineHeight and
        // convert to a Cairo baseline by adding the font ascent.
        const auto text_size = balloon.text_size > 0.0
                                   ? balloon.text_size * transform.scale
                                   : static_cast<double>(balloon.line_height) * transform.scale * 0.72;
        if (text_size >= 1.0 && !balloon.lines.empty()) {
            set_color(context, {0.08, 0.07, 0.08, 1.0});
            auto* face = static_cast<FT_Face>(text.native_face());
            auto* cairo_face = cairo_ft_font_face_create_for_ft_face(face, 0);
            cairo_set_font_face(context, cairo_face);
            cairo_set_font_size(context, text_size);
            cairo_font_face_destroy(cairo_face);
            cairo_font_extents_t extents{};
            cairo_font_extents(context, &extents);

            // Per-line left x == ShiftLines' rgiLeftX (balloon.cpp:768): centered
            // balloons offset each line by (maxWidth - width_i)/2 from the text
            // origin (bbox.left, the text-frame x=0 after SetBBox), the action box
            // is FT_LEFT_JUSTIFY at offset 0. This is the source's own text origin,
            // NOT the cloud-outline center -- centering on the cloud center would
            // bias text by XBORDER and clip the widest line. Drawing left-anchored
            // there keeps every line inside the XBORDER text margin of the cloud.
            const bool left_justify = balloon.kind.mode == BalloonMode::action;
            const auto max_line_width = widest_line_width(balloon.lines);
            auto cell_top_y = balloon.bbox.top;
            for (const auto& line : balloon.lines) {
                const auto offset = left_justify ? 0 : (max_line_width - line.width) / 2;
                const auto left_x = balloon.bbox.left + offset;
                const auto anchor = transform.to_device(
                    LogicalPoint{static_cast<double>(left_x), static_cast<double>(cell_top_y)});
                const double baseline = anchor.y + extents.ascent;
                if (line.runs.empty()) {
                    // ZERO-BEHAVIOR-CHANGE PATH (the common case): no per-run
                    // formatting -> the exact single-color draw_shaped call as
                    // before this feature, byte-identical rendered pixels.
                    draw_shaped(context, text, line.text, text_size, anchor.x, baseline,
                                /*centered=*/false);
                } else {
                    // FORMATTED PATH: split the line into segments at run offsets
                    // (line-local bytes) and draw each with its own ink + synthetic
                    // bold/italic/underline, advancing the pen by each segment's
                    // measured x-advance. draw_run_glyphs saves/restores cairo state
                    // so no font matrix / color / line width bleeds across segments.
                    double pen_x = anchor.x;
                    const auto& runs = line.runs;
                    // A leading default-format segment when the first run starts
                    // past byte 0 (text before any formatting on this line).
                    if (runs.front().offset > 0) {
                        const auto head = std::string_view{line.text}.substr(0, runs.front().offset);
                        pen_x += draw_run_glyphs(context, text, head, text_size, pen_x, baseline,
                                                 default_balloon_ink, /*bold=*/false, /*italic=*/false,
                                                 /*underline=*/false);
                    }
                    for (std::size_t r = 0; r < runs.size(); ++r) {
                        const std::size_t start = std::min(runs[r].offset, line.text.size());
                        const std::size_t end =
                            (r + 1 < runs.size()) ? std::min(runs[r + 1].offset, line.text.size())
                                                  : line.text.size();
                        if (end <= start) continue;
                        const auto span = std::string_view{line.text}.substr(start, end - start);
                        pen_x += draw_run_glyphs(context, text, span, text_size, pen_x, baseline,
                                                 run_ink(runs[r]), runs[r].bold, runs[r].italic,
                                                 runs[r].underline || runs[r].link);
                    }
                }
                cell_top_y -= balloon.line_height;
            }
        }
    }

    // CUnitPanel::DrawBorder (panel.cpp:713): the panel frame, a closed rectangle
    // [0,0]..[m_unitWidth,-m_unitHeight] stroked with m_borderPen
    // (PS_SOLID, 2 * m_borderWidth, black). Drawn last, still under the panel clip,
    // so the pen's outer half is trimmed exactly as the source and the inner half
    // frames the panel content.
    cairo_new_path(context);
    cairo_rectangle(context, panel_tl.x, panel_tl.y, panel_br.x - panel_tl.x, panel_br.y - panel_tl.y);
    cairo_set_source_rgba(context, 0.0, 0.0, 0.0, 1.0);
    cairo_set_line_width(context, std::max(1.0, 2.0 * source_border_width * transform.scale));
    cairo_set_dash(context, nullptr, 0, 0.0);
    cairo_stroke(context);

    cairo_restore(context);
    cairo_surface_flush(impl_->surface.get());
}

void Canvas::render_page(std::span<const Panel> panels, TextEngine& text,
                         const PanelAvatarProvider& avatars) {
    if (panels.empty()) {
        return;
    }
    auto* context = impl_->context.get();
    const auto canvas_w = static_cast<double>(impl_->width);
    const auto canvas_h = static_cast<double>(impl_->height);

    // Whole-page bounds in panel twips (Y-up: top == 0, rows extend downward),
    // then fit the page into the canvas centered and aspect-preserving.
    const auto bounds = page_bounds(panels.size(), 0, 0);
    const auto page_w = static_cast<double>(bounds.right - bounds.left);
    const auto page_h = static_cast<double>(bounds.top - bounds.bottom);
    const auto page_scale = std::min(canvas_w / page_w, canvas_h / page_h);
    const auto page_origin_x = (canvas_w - page_w * page_scale) / 2.0;
    const auto page_origin_y = (canvas_h - page_h * page_scale) / 2.0;

    // render_panel always fits its 2300-twip square into the FULL canvas
    // (fit_panel_transform), producing this device square. The per-cell transform
    // below remaps that fixed square onto each cell's device rect, so a lone
    // full-page cell (single-panel strip) yields the identity transform and the
    // draw is byte-identical to render_panel.
    const auto rp_square = std::min(canvas_w, canvas_h);
    const auto rp_origin_x = (canvas_w - rp_square) / 2.0;
    const auto rp_origin_y = (canvas_h - rp_square) / 2.0;

    for (std::size_t i = 0; i < panels.size(); ++i) {
        const auto cell = panel_rect(i, 0, 0);
        const auto cell_x = page_origin_x + static_cast<double>(cell.left - bounds.left) * page_scale;
        const auto cell_y = page_origin_y + static_cast<double>(bounds.top - cell.top) * page_scale;
        const auto cell_w = static_cast<double>(cell.right - cell.left) * page_scale;
        const auto cell_h = static_cast<double>(cell.top - cell.bottom) * page_scale;

        cairo_save(context);
        // Map render_panel's full-canvas 2300-square onto this cell's device rect.
        cairo_translate(context, cell_x, cell_y);
        cairo_scale(context, cell_w / rp_square, cell_h / rp_square);
        cairo_translate(context, -rp_origin_x, -rp_origin_y);
        render_panel(panels[i], text, avatars);
        cairo_restore(context);
    }
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
