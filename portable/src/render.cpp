#include "comicchat/render.hpp"

#include "comicchat/text.hpp"

#include <algorithm>
#include <cmath>
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

auto checked_pixel_count(const std::int32_t width, const std::int32_t height) -> std::size_t {
    if (width <= 0 || height <= 0) throw std::invalid_argument{"canvas dimensions must be positive"};
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
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
        : width{requested_width}, height{requested_height}, data(checked_pixel_count(width, height)) {
        surface = cairo_image_surface_create_for_data(
            reinterpret_cast<unsigned char*>(data.data()), CAIRO_FORMAT_ARGB32, width, height, width * 4);
        if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) throw std::runtime_error{"Cairo surface creation failed"};
        context = cairo_create(surface);
        if (cairo_status(context) != CAIRO_STATUS_SUCCESS) throw std::runtime_error{"Cairo context creation failed"};
    }

    ~Impl() {
        if (context != nullptr) cairo_destroy(context);
        if (surface != nullptr) cairo_surface_destroy(surface);
    }

    std::int32_t width{};
    std::int32_t height{};
    std::vector<std::uint32_t> data;
    cairo_surface_t* surface{};
    cairo_t* context{};
};

Canvas::Canvas(const std::int32_t width, const std::int32_t height) : impl_{std::make_unique<Impl>(width, height)} {}
Canvas::~Canvas() = default;
Canvas::Canvas(Canvas&&) noexcept = default;
auto Canvas::operator=(Canvas&&) noexcept -> Canvas& = default;
auto Canvas::width() const noexcept -> std::int32_t { return impl_->width; }
auto Canvas::height() const noexcept -> std::int32_t { return impl_->height; }
auto Canvas::pixels() const noexcept -> std::span<const std::uint32_t> { return impl_->data; }

void Canvas::clear(const Rgba color) {
    cairo_save(impl_->context);
    cairo_set_operator(impl_->context, CAIRO_OPERATOR_SOURCE);
    set_color(impl_->context, color);
    cairo_paint(impl_->context);
    cairo_restore(impl_->context);
    cairo_surface_flush(impl_->surface);
}

void Canvas::render_title_panel(const TitlePanel& model, TextEngine& text) {
    auto* context = impl_->context;
    const auto panel_size = static_cast<double>(std::min(impl_->width, impl_->height));
    const auto scale = panel_size / source_panel_units;
    const auto origin_x = (static_cast<double>(impl_->width) - panel_size) / 2.0;
    const auto origin_y = (static_cast<double>(impl_->height) - panel_size) / 2.0;
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
    cairo_surface_flush(impl_->surface);
}

auto Canvas::write_png(const std::string_view path) const -> bool {
    const std::string owned_path{path};
    return cairo_surface_write_to_png(impl_->surface, owned_path.c_str()) == CAIRO_STATUS_SUCCESS;
}

} // namespace comicchat
