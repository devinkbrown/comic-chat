#include "comicchat/comic_page.hpp"
#include "comicchat/config.hpp"
#include "comicchat/net/ircv3.hpp"
#include "comicchat/net/native_session.hpp"
#include "comicchat/page.hpp"
#include "comicchat/render.hpp"
#include "comicchat/text.hpp"

#include "compose_input.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

// The compose bar (this file's ComposeBar) draws directly with Cairo + the
// public TextEngine::shape()/native_face() API, the same technique
// render.cpp's internal draw_shaped() uses. Canvas (render.hpp) has no public
// "draw arbitrary text/rect" primitive to reuse — its surface is a comic
// panel, not app chrome — so the compose bar owns a second, independent
// Cairo ARGB32 surface for its own small overlay strip and never touches
// Canvas's pixels.
#include <cairo-ft.h>
#include <cairo.h>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace {

struct Sdl final {
    Sdl() {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
            throw std::runtime_error{SDL_GetError()};
        }
    }
    ~Sdl() { SDL_Quit(); }
};

struct WindowDeleter final { void operator()(SDL_Window* value) const noexcept { SDL_DestroyWindow(value); } };
struct RendererDeleter final { void operator()(SDL_Renderer* value) const noexcept { SDL_DestroyRenderer(value); } };
struct TextureDeleter final { void operator()(SDL_Texture* value) const noexcept { SDL_DestroyTexture(value); } };
struct SurfaceDeleter final { void operator()(SDL_Surface* value) const noexcept { SDL_DestroySurface(value); } };
struct CairoSurfaceDeleter final { void operator()(cairo_surface_t* value) const noexcept { cairo_surface_destroy(value); } };
struct CairoContextDeleter final { void operator()(cairo_t* value) const noexcept { cairo_destroy(value); } };

struct ChatLine final {
    std::string nick;
    std::string text;
};

// RAII guard around SDL3's per-window text-input subsystem
// (SDL_StartTextInput/SDL_StopTextInput). Construction failure is logged, not
// thrown: a headless/dummy video driver run (--frames/--png) must keep working
// even if the backend has no IME/text-input support to start, since it never
// waits on text-input events either way.
class TextInputSession final {
public:
    explicit TextInputSession(SDL_Window* window) : window_{window} {
        if (!SDL_StartTextInput(window_)) {
            std::cerr << "Comic Chat could not begin text input: " << SDL_GetError() << '\n';
        }
    }
    ~TextInputSession() { (void)SDL_StopTextInput(window_); }
    TextInputSession(const TextInputSession&) = delete;
    auto operator=(const TextInputSession&) -> TextInputSession& = delete;
    TextInputSession(TextInputSession&&) = delete;
    auto operator=(TextInputSession&&) -> TextInputSession& = delete;

private:
    SDL_Window* window_;
};

// Compose-bar layout constants, in device pixels (matching the canvas's own
// SDL_GetWindowSizeInPixels device-pixel coordinate space, so the bar and the
// comic panel composite with one consistent scale).
constexpr double compose_bar_height = 56.0;
constexpr double compose_bar_padding = 14.0;
constexpr double compose_bar_font_size = 24.0;
constexpr Uint64 caret_blink_interval_ms = 500;

// The rasterized compose bar, ready for SDL_UpdateTexture: premultiplied
// ARGB32 pixels (matching both Cairo's native layout and
// SDL_PIXELFORMAT_ARGB8888, the same format the main canvas texture already
// uses) plus the row stride SDL_UpdateTexture needs.
struct ComposeBarSurface final {
    std::vector<std::uint32_t> pixels;
    int width{};
    int height{};
    int stride_bytes{};
};

// Total glyph advance of `glyphs`, mirroring render.cpp's local
// shape_advance() (not reusable here: it is an internal, non-exported detail
// of render.cpp, not part of Canvas's public API).
auto shape_advance(const std::vector<comicchat::ShapedGlyph>& glyphs) -> double {
    double advance{};
    for (const auto& glyph : glyphs) advance += glyph.x_advance;
    return advance;
}

// The x-advance `value` occupies when shaped at `size` (0 on shaping
// failure), used to place the compose-bar caret and to scroll long lines.
auto measure_text(comicchat::TextEngine& text, std::string_view value, double size) -> double {
    const auto shaped = text.shape(value, size);
    return shaped ? shape_advance(*shaped) : 0.0;
}

// Draws `value` left-aligned so its first glyph starts at (x, baseline),
// mirroring render.cpp's local draw_shaped() technique (Cairo positioned via
// the shared TextEngine::shape()/native_face() API) for this independent
// compose-bar surface.
void draw_text(cairo_t* context, comicchat::TextEngine& text, std::string_view value, double size, double x,
               double baseline) {
    const auto shaped = text.shape(value, size);
    if (!shaped || shaped->empty()) return;
    auto* face = static_cast<FT_Face>(text.native_face());
    auto* cairo_face = cairo_ft_font_face_create_for_ft_face(face, 0);
    cairo_set_font_face(context, cairo_face);
    cairo_set_font_size(context, size);
    cairo_font_face_destroy(cairo_face);
    std::vector<cairo_glyph_t> glyphs;
    glyphs.reserve(shaped->size());
    double cursor = x;
    for (const auto& glyph : *shaped) {
        glyphs.push_back({glyph.index, cursor + glyph.x_offset, baseline - glyph.y_offset});
        cursor += glyph.x_advance;
    }
    cairo_show_glyphs(context, glyphs.data(), static_cast<int>(glyphs.size()));
}

// Renders the compose bar (background, border, current text scrolled so the
// caret stays visible, and an optional blinking caret) into a fresh Cairo
// ARGB32 surface `window_width` wide. Never throws: a shaping failure just
// leaves that glyph run undrawn, matching render.cpp's draw_shaped()
// best-effort behavior.
auto render_compose_bar(comicchat::TextEngine& text, const comicchat::ComposeBuffer& buffer, int window_width,
                        bool caret_visible) -> ComposeBarSurface {
    const int width = std::max(window_width, 1);
    const int height = static_cast<int>(compose_bar_height);
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    const auto stride_pixels = static_cast<std::size_t>(stride) / sizeof(std::uint32_t);

    ComposeBarSurface result;
    result.width = width;
    result.height = height;
    result.stride_bytes = stride;
    result.pixels.assign(stride_pixels * static_cast<std::size_t>(height), 0);

    std::unique_ptr<cairo_surface_t, CairoSurfaceDeleter> surface{cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(result.pixels.data()), CAIRO_FORMAT_ARGB32, width, height, stride)};
    if (!surface || cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error{"Cairo compose-bar surface creation failed"};
    }
    std::unique_ptr<cairo_t, CairoContextDeleter> context{cairo_create(surface.get())};
    if (!context || cairo_status(context.get()) != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error{"Cairo compose-bar context creation failed"};
    }

    cairo_set_source_rgba(context.get(), 0.98, 0.98, 0.96, 1.0);
    cairo_paint(context.get());
    cairo_set_source_rgba(context.get(), 0.16, 0.15, 0.18, 1.0);
    cairo_set_line_width(context.get(), 2.0);
    cairo_move_to(context.get(), 0.0, 1.0);
    cairo_line_to(context.get(), static_cast<double>(width), 1.0);
    cairo_stroke(context.get());
    cairo_rectangle(context.get(), 1.0, 3.0, std::max(0.0, width - 2.0), std::max(0.0, height - 6.0));
    cairo_stroke(context.get());

    const double baseline = static_cast<double>(height) / 2.0 + compose_bar_font_size * 0.36;
    const double text_left = compose_bar_padding;
    const double text_width = std::max(0.0, static_cast<double>(width) - 2.0 * compose_bar_padding);
    const double caret_advance =
        measure_text(text, buffer.text().substr(0, buffer.cursor_offset()), compose_bar_font_size);
    const double scroll = std::max(0.0, caret_advance - text_width);

    cairo_save(context.get());
    cairo_rectangle(context.get(), text_left, 0.0, text_width, static_cast<double>(height));
    cairo_clip(context.get());
    cairo_set_source_rgba(context.get(), 0.08, 0.07, 0.08, 1.0);
    draw_text(context.get(), text, buffer.text(), compose_bar_font_size, text_left - scroll, baseline);
    if (caret_visible) {
        const double caret_x = text_left - scroll + caret_advance;
        cairo_set_line_width(context.get(), 2.0);
        cairo_move_to(context.get(), caret_x, baseline - compose_bar_font_size * 0.82);
        cairo_line_to(context.get(), caret_x, baseline + compose_bar_font_size * 0.18);
        cairo_stroke(context.get());
    }
    cairo_restore(context.get());
    cairo_surface_flush(surface.get());
    return result;
}

struct Arguments final {
    std::optional<std::string> font;
    std::optional<std::string> png;
    std::optional<comicchat::ConnectionConfig> connection;
    std::optional<ChatLine> say;  // synthetic message for headless --png demos
    std::uint64_t frames{};
};

constexpr std::string_view app_name = "Comic Chat: Reinked";
constexpr std::string_view app_id = "io.github.devinkbrown.ComicChatReinked";
constexpr std::array modern_icon_sizes{32, 64, 128, 256};

#if defined(COMICCHAT_VERSION)
constexpr std::string_view app_version = COMICCHAT_VERSION;
#else
constexpr std::string_view app_version = "0.1.0-dev";
#endif

auto parse_arguments(const int argc, char** argv) -> std::optional<Arguments> {
    Arguments result;
    std::vector<std::string_view> connection_arguments;
    bool connection_marker{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if ((argument == "--font" || argument == "--png" || argument == "--frames") && index + 1 >= argc) {
            return std::nullopt;
        }
        if (argument == "--say" && index + 2 >= argc) return std::nullopt;
        if (argument == "--font") result.font = argv[++index];
        else if (argument == "--png") result.png = argv[++index];
        else if (argument == "--say") {
            std::string nick = argv[++index];
            result.say = ChatLine{std::move(nick), argv[++index]};
        } else if (argument == "--frames") {
            const std::string_view value{argv[++index]};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result.frames);
            if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
        } else if (argument == "--help") {
            std::cout << "comic-chat [--font FILE] [--png FILE] [--frames COUNT] [--say NICK TEXT] "
                         "[--connect HOST [PORT] NICK CHANNEL [--tls|--plaintext] "
                         "[--ca-file FILE]]\n";
            return Arguments{
                .font = std::string{},
                .png = std::string{},
                .connection = std::nullopt,
                .say = std::nullopt,
                .frames = 0,
            };
        } else if (argument == "--connect") {
            if (connection_marker) return std::nullopt;
            connection_marker = true;
        } else if (connection_marker) {
            connection_arguments.push_back(argument);
        } else {
            return std::nullopt;
        }
    }
    if (connection_marker) {
        result.connection = comicchat::parse_connection_args(connection_arguments);
        if (!result.connection) return std::nullopt;
    }
    return result;
}

auto model() -> comicchat::TitlePanel {
    return {
        "A Chat to Remember",
        "STARRING",
        {
            {"You", 0x456990U, false},
            {"Ada", 0xb85c5cU, false},
            {"Linus", 0x6b986bU, false},
        },
    };
}

// The speaker nick is the prefix up to '!' (nick!user@host); a serverless
// prefix (no '!') is used whole. Empty when the message carries no prefix.
auto nick_from_prefix(std::string_view prefix) -> std::string_view {
    const auto bang = prefix.find('!');
    return prefix.substr(0, bang == std::string_view::npos ? prefix.size() : bang);
}

auto ascii_iequals(std::string_view lhs, std::string_view rhs) -> bool {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        const auto lower = [](char value) {
            return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
        };
        if (lower(lhs[index]) != lower(rhs[index])) return false;
    }
    return true;
}

// A PRIVMSG addressed to `channel` becomes one comic line. CTCP ACTION and other
// commands (NOTICE, JOIN, ...) are Phase 2.5b work and are ignored here.
auto channel_line(const comic_chat::ircv3::Message& message, std::string_view channel)
    -> std::optional<ChatLine> {
    if (!ascii_iequals(message.command, "PRIVMSG") || message.params.size() < 2) return std::nullopt;
    if (!ascii_iequals(message.params[0], channel)) return std::nullopt;
    const std::string_view prefix = message.prefix ? std::string_view{*message.prefix} : std::string_view{};
    const std::string_view nick = nick_from_prefix(prefix);
    if (nick.empty() || message.params[1].empty()) return std::nullopt;
    return ChatLine{std::string{nick}, message.params[1]};
}

auto complete_icon_ladder(const std::filesystem::path& root) -> bool {
    std::error_code error;
    for (const int size : modern_icon_sizes) {
        const auto candidate = root / "chat" / (std::to_string(size) + ".png");
        if (!std::filesystem::is_regular_file(candidate, error)) return false;
        error.clear();
    }
    return true;
}

auto modern_icon_directories() -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> result;
    const auto append = [&](std::filesystem::path candidate) {
        if (candidate.empty()) return;
        candidate = candidate.lexically_normal();
        if (std::find(result.begin(), result.end(), candidate) == result.end()) {
            result.push_back(std::move(candidate));
        }
    };
    if (const char* configured = std::getenv("COMICCHAT_MODERN_ICON_DIR")) append(configured);
#if defined(COMICCHAT_INSTALL_MODERN_ICON_DIR)
    append(COMICCHAT_INSTALL_MODERN_ICON_DIR);
#endif
    if (const char* base = SDL_GetBasePath()) {
        append(std::filesystem::path{base} / ".." / "share" / "comic-chat-reinked" / "icons");
    }
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    if (!error) {
        append(current / "portable" / "assets" / "icons" / "generated" / "png");
        append(current / "assets" / "icons" / "generated" / "png");
        append(current / ".." / "portable" / "assets" / "icons" / "generated" / "png");
    }
    return result;
}

auto load_modern_icon_ladder(const std::filesystem::path& root) -> std::unique_ptr<SDL_Surface, SurfaceDeleter> {
    const auto base_path = root / "chat" / "32.png";
    std::unique_ptr<SDL_Surface, SurfaceDeleter> base{SDL_LoadPNG(base_path.string().c_str())};
    if (!base || base->w != 32 || base->h != 32) return {};
    for (std::size_t index = 1; index < modern_icon_sizes.size(); ++index) {
        const int size = modern_icon_sizes[index];
        const auto path = root / "chat" / (std::to_string(size) + ".png");
        std::unique_ptr<SDL_Surface, SurfaceDeleter> alternate{SDL_LoadPNG(path.string().c_str())};
        if (!alternate || alternate->w != size || alternate->h != size ||
            !SDL_AddSurfaceAlternateImage(base.get(), alternate.get())) {
            return {};
        }
    }
    return base;
}

void apply_modern_window_icon(SDL_Window* window) {
    for (const auto& root : modern_icon_directories()) {
        if (!complete_icon_ladder(root)) continue;
        auto icon = load_modern_icon_ladder(root);
        if (icon && SDL_SetWindowIcon(window, icon.get())) return;
    }
    std::cerr << "Comic Chat could not load its 32/64/128/256 modern icon ladder: "
              << SDL_GetError() << '\n';
}

void configure_app_metadata() {
    if (!SDL_SetAppMetadata(app_name.data(), app_version.data(), app_id.data())) {
        std::cerr << "Comic Chat could not set SDL application metadata: " << SDL_GetError() << '\n';
    }
    (void)SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_CREATOR_STRING,
                                     "Comic Chat: Reinked contributors");
    (void)SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_COPYRIGHT_STRING,
                                     "Microsoft Corporation and Comic Chat: Reinked contributors");
    (void)SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_URL_STRING,
                                     "https://github.com/devinkbrown/comic-chat");
    (void)SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_TYPE_STRING, "application");
}

} // namespace

auto main(const int argc, char** argv) -> int {
    try {
        const auto arguments = parse_arguments(argc, argv);
        if (!arguments) {
            std::cerr << "usage: comic-chat [--font FILE] [--png FILE] [--frames COUNT] [--say NICK TEXT] "
                         "[--connect HOST [PORT] NICK CHANNEL [--tls|--plaintext] "
                         "[--ca-file FILE]]\n";
            return 2;
        }
        if (arguments->font && arguments->font->empty() && arguments->png && arguments->png->empty()) return 0;
        const auto font_path = arguments->font ? std::expected<std::string, comicchat::TextError>{*arguments->font}
                                               : comicchat::find_portable_comic_font();
        if (!font_path) {
            std::cerr << "Comic Chat could not locate a TrueType/OpenType font\n";
            return 1;
        }
        auto text = comicchat::TextEngine::create(*font_path);
        if (!text) {
            std::cerr << "Comic Chat could not load the selected font\n";
            return 1;
        }

        configure_app_metadata();
        Sdl sdl;
        std::unique_ptr<comicchat::net::NativeSession> session;
        if (arguments->connection) {
            const auto paths = comicchat::net::prepare_native_user_paths();
            if (!paths) throw std::runtime_error{"could not create private per-user state directories"};
            session = std::make_unique<comicchat::net::NativeSession>(paths->sts_policy_file);
            const auto wakeup_event = SDL_RegisterEvents(1);
            if (wakeup_event == 0) throw std::runtime_error{SDL_GetError()};
            session->set_wakeup([wakeup_event] {
                SDL_Event event{};
                event.type = wakeup_event;
                (void)SDL_PushEvent(&event);
            });
            comicchat::net::NativeSessionOptions session_options;
            session_options.connection.endpoint = {arguments->connection->host, arguments->connection->port};
            session_options.connection.server_name = arguments->connection->host;
            session_options.connection.security = arguments->connection->security == comicchat::Security::tls
                                                      ? comicchat::net::Security::tls
                                                      : comicchat::net::Security::plaintext;
            session_options.connection.ca_file = arguments->connection->ca_file;
            session_options.nickname = arguments->connection->nickname;
            session_options.channel = arguments->connection->channel;
            if (!session->start(std::move(session_options)))
                throw std::runtime_error{"could not start the native IRC session"};
        }
        SDL_Window* raw_window{};
        SDL_Renderer* raw_renderer{};
        if (!SDL_CreateWindowAndRenderer(app_name.data(), 760, 760,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                        &raw_window, &raw_renderer)) {
            throw std::runtime_error{SDL_GetError()};
        }
        std::unique_ptr<SDL_Window, WindowDeleter> window{raw_window};
        std::unique_ptr<SDL_Renderer, RendererDeleter> renderer{raw_renderer};
        apply_modern_window_icon(window.get());
        // Owns SDL_StartTextInput()/SDL_StopTextInput() for the window's whole
        // lifetime. A headless --frames/--png run still starts and stops it
        // (harmlessly, even under the dummy video driver): it never blocks on a
        // text-input event either way, satisfying "headless must not require
        // text input" without a separate code path.
        TextInputSession text_input{window.get()};
        std::unique_ptr<SDL_Texture, TextureDeleter> texture;
        std::unique_ptr<comicchat::Canvas> canvas;
        std::unique_ptr<SDL_Texture, TextureDeleter> compose_texture;

        // The faithful page-composition model (CUnitPanelPage::AddLine, panel.cpp:1061):
        // chat lines accumulate into `page`, which runs the real LayoutAvatars /
        // LayoutBalloons expert placement and emits framed comic Panels. The live
        // view shows the tail (current) panel; empty until the first line, so the
        // title card holds on an idle connection.
        const auto page_font = comicchat::build_font_metrics(**text, comicchat::message_text_size, 0, 0);
        comicchat::PageConfig page_config;
        page_config.font = page_font.value_or(comicchat::FontMetrics{});
        page_config.text_size = comicchat::message_text_size;
        page_config.max_text_width = comicchat::message_balloon_max_width;
        comicchat::Page page{page_config, comicchat::measure_text_width(**text, comicchat::message_text_size)};

        // The most recent assembled panel (page.panels().back()).
        std::optional<comicchat::Panel> current_panel;
        // Per-speaker avatar providers, keyed by the PageAvatar id a body carries,
        // so each body in a multi-speaker panel composites its OWN speaker's
        // avatar. Empty entries (no avatar set resolves) leave the color box.
        std::map<std::uint32_t, comicchat::PanelAvatarProvider> avatar_providers;
        const comicchat::PanelAvatarProvider current_avatar =
            [&avatar_providers](const comicchat::PanelBody& body, std::int32_t target_width,
                                std::int32_t target_height) -> std::optional<comicchat::AvatarBitmap> {
            const auto found = avatar_providers.find(body.avatar_id);
            if (found == avatar_providers.end() || !found->second) return std::nullopt;
            return found->second(body, target_width, target_height);
        };

        // Feed one chat line into the page and refresh the current panel + the
        // speaker's avatar provider (deterministic nick -> id/color/avatar).
        const auto feed_line = [&](std::string_view nick, std::string_view line_text) {
            const auto avatar_id = comicchat::nick_avatar_id(nick);
            comicchat::PageAvatar speaker;
            speaker.avatar_id = avatar_id;
            speaker.body_width = comicchat::message_body_width;
            speaker.body_height = comicchat::message_body_height;
            speaker.face_fraction = 0.5;
            speaker.color = comicchat::nick_color(nick);
            if (auto provider = comicchat::make_nick_avatar_provider(nick)) {
                avatar_providers[avatar_id] = std::move(provider);
            }
            page.add_line(comicchat::Line{speaker, std::string{line_text}, comicchat::bm_say});
            if (!page.panels().empty()) current_panel = page.panels().back();
        };

        // The local user's own nick for compose-bar submissions: the connected
        // session's nickname when live, otherwise the title panel's "You" star
        // (model() above) so an offline compose still labels its own lines
        // consistently with the idle title card.
        const std::string local_nickname = arguments->connection ? arguments->connection->nickname : "You";

        // The message-composition buffer (Phase 2.5c): SDL3 text-input events
        // and editing keys (Backspace/Left/Right/Home/End/Escape) mutate this;
        // Enter submits it. Purely local UI state, independent of `page`.
        comicchat::ComposeBuffer compose;
        bool caret_visible = true;

        // Repaints the compose bar's own Cairo surface into `compose_texture`
        // from the buffer's current text/cursor. Safe to call before the first
        // rebuild() (compose_texture/canvas are still null then, so it is a
        // no-op) so callers never need to special-case startup ordering.
        const auto render_compose_into_texture = [&] {
            if (!compose_texture || !canvas) return;
            const auto surface = render_compose_bar(**text, compose, canvas->width(), caret_visible);
            if (!SDL_UpdateTexture(compose_texture.get(), nullptr, surface.pixels.data(), surface.stride_bytes)) {
                throw std::runtime_error{SDL_GetError()};
            }
        };

        const auto rebuild = [&] {
            int width{};
            int height{};
            if (!SDL_GetWindowSizeInPixels(window.get(), &width, &height)) throw std::runtime_error{SDL_GetError()};
            width = std::max(width, 1);
            height = std::max(height, 1);
            canvas = std::make_unique<comicchat::Canvas>(width, height);
            canvas->clear({1.0, 1.0, 1.0, 1.0});
            if (current_panel) {
                canvas->render_panel(*current_panel, **text, current_avatar);
            } else {
                canvas->render_title_panel(model(), **text);
            }
            texture.reset(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_STREAMING, width, height));
            if (!texture) throw std::runtime_error{SDL_GetError()};
            if (!SDL_UpdateTexture(texture.get(), nullptr, canvas->pixels().data(), width * 4)) {
                throw std::runtime_error{SDL_GetError()};
            }

            const int compose_height = std::min(height, static_cast<int>(compose_bar_height));
            compose_texture.reset(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888,
                                                     SDL_TEXTUREACCESS_STREAMING, width, compose_height));
            if (!compose_texture) throw std::runtime_error{SDL_GetError()};
            render_compose_into_texture();

            // Hint the native IME/candidate window at the compose bar, in window
            // (not device-pixel) coordinates per SDL_SetTextInputArea's contract.
            int window_width{};
            int window_height{};
            if (SDL_GetWindowSize(window.get(), &window_width, &window_height)) {
                const float density = SDL_GetWindowPixelDensity(window.get());
                const float scale = density > 0.0f ? density : 1.0f;
                const int area_height = static_cast<int>(static_cast<float>(compose_height) / scale);
                const SDL_Rect area{0, std::max(0, window_height - area_height), window_width, area_height};
                (void)SDL_SetTextInputArea(window.get(), &area, 0);
            }
        };
        // A synthetic --say line drives the same page-composition path the live
        // feed uses, so a headless --png snapshot shows a real framed comic panel.
        if (arguments->say) {
            feed_line(arguments->say->nick, arguments->say->text);
        }
        rebuild();
        if (arguments->png && !arguments->png->empty() && !canvas->write_png(*arguments->png)) {
            std::cerr << "could not write PNG snapshot\n";
            return 1;
        }

        bool running = true;
        bool redraw = true;
        std::uint64_t rendered{};
        Uint64 next_caret_toggle = SDL_GetTicks() + caret_blink_interval_ms;
        while (running) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) running = false;
                else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    rebuild();
                    redraw = true;
                } else if (event.type == SDL_EVENT_TEXT_INPUT) {
                    // event.text.text is an SDL-owned, UTF-8 C string valid only
                    // for this event; insert() copies it into the buffer
                    // immediately, so nothing retains the pointer past this branch.
                    compose.insert(event.text.text != nullptr ? std::string_view{event.text.text}
                                                               : std::string_view{});
                    caret_visible = true;
                    next_caret_toggle = SDL_GetTicks() + caret_blink_interval_ms;
                    render_compose_into_texture();
                    redraw = true;
                } else if (event.type == SDL_EVENT_KEY_DOWN) {
                    const bool ctrl = (event.key.mod & SDL_KMOD_CTRL) != 0;
                    bool compose_touched = true;
                    if (event.key.key == SDLK_C && ctrl) {
                        (void)SDL_SetClipboardText("A Chat to Remember — Microsoft Comic Chat");
                        compose_touched = false;
                    } else if (event.key.key == SDLK_BACKSPACE) {
                        compose.backspace();
                    } else if (event.key.key == SDLK_LEFT) {
                        compose.move_left();
                    } else if (event.key.key == SDLK_RIGHT) {
                        compose.move_right();
                    } else if (event.key.key == SDLK_HOME) {
                        compose.move_home();
                    } else if (event.key.key == SDLK_END) {
                        compose.move_end();
                    } else if (event.key.key == SDLK_ESCAPE) {
                        compose.clear();
                    } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
                        if (compose.empty()) {
                            compose_touched = false;
                        } else {
                            const auto submitted = compose.take();
                            // Feeds the same Page::add_line() path the live IRC
                            // feed and --say both use, so the submitted line
                            // becomes a real framed comic panel immediately.
                            feed_line(local_nickname, submitted);
                            // NOTE (Phase 2.5c honest limit): NativeSession only
                            // exposes start()/poll()/set_wakeup()/stop() — there is
                            // no public outbound-send API yet to also PRIVMSG this
                            // line over the wire when `session` is connected. That
                            // requires a NativeSession send surface, which is a
                            // net/native_session.hpp/.cpp change outside this
                            // change's file scope; the compose bar stays
                            // local-echo-only against a live session until that
                            // lands.
                            // rebuild() below also repaints compose_texture (now
                            // empty) against the freshly sized canvas, so the
                            // shared compose_touched branch below does not need to
                            // redo that work.
                            caret_visible = true;
                            next_caret_toggle = SDL_GetTicks() + caret_blink_interval_ms;
                            rebuild();
                            compose_touched = false;
                            redraw = true;
                        }
                    } else {
                        compose_touched = false;
                    }
                    if (compose_touched) {
                        caret_visible = true;
                        next_caret_toggle = SDL_GetTicks() + caret_blink_interval_ms;
                        render_compose_into_texture();
                        redraw = true;
                    }
                }
            }
            if (const auto now = SDL_GetTicks(); now >= next_caret_toggle) {
                caret_visible = !caret_visible;
                next_caret_toggle = now + caret_blink_interval_ms;
                render_compose_into_texture();
                redraw = true;
            }
            if (session) {
                const auto network = session->poll(64, 512);
                for (const auto& diagnostic : network.diagnostics) {
                    std::cerr << "IRC session [" << diagnostic.code << "]: " << diagnostic.message << '\n';
                }
                // Each channel line is accumulated into the page (AddLine): the
                // real multi-body / multi-balloon panel composition, with the tail
                // panel shown live.
                for (const auto& message : network.messages) {
                    if (auto line = channel_line(message, arguments->connection->channel)) {
                        feed_line(line->nick, line->text);
                        rebuild();
                        redraw = true;
                    }
                }
            }
            if (redraw || arguments->frames != 0) {
                SDL_SetRenderDrawColor(renderer.get(), 255, 255, 255, 255);
                SDL_RenderClear(renderer.get());
                SDL_RenderTexture(renderer.get(), texture.get(), nullptr, nullptr);
                // The compose bar draws last, on top of the comic panel, pinned to
                // the bottom of the window in the same device-pixel space `canvas`
                // uses.
                const auto compose_height =
                    static_cast<float>(std::min(canvas->height(), static_cast<std::int32_t>(compose_bar_height)));
                const SDL_FRect compose_dst{0.0f, static_cast<float>(canvas->height()) - compose_height,
                                            static_cast<float>(canvas->width()), compose_height};
                SDL_RenderTexture(renderer.get(), compose_texture.get(), nullptr, &compose_dst);
                SDL_RenderPresent(renderer.get());
                redraw = false;
                ++rendered;
                if (arguments->frames != 0 && rendered >= arguments->frames) running = false;
            } else {
                SDL_WaitEventTimeout(nullptr, 16);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Comic Chat: " << error.what() << '\n';
        return 1;
    }
}
