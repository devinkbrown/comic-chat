#include "comicchat/comic_page.hpp"
#include "comicchat/config.hpp"
#include "comicchat/net/ircv3.hpp"
#include "comicchat/net/native_session.hpp"
#include "comicchat/page.hpp"
#include "comicchat/render.hpp"
#include "comicchat/text.hpp"

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

struct ChatLine final {
    std::string nick;
    std::string text;
};

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
        std::unique_ptr<SDL_Texture, TextureDeleter> texture;
        std::unique_ptr<comicchat::Canvas> canvas;

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
        while (running) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) running = false;
                else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    rebuild();
                    redraw = true;
                } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_C &&
                           (event.key.mod & SDL_KMOD_CTRL) != 0) {
                    (void)SDL_SetClipboardText("A Chat to Remember — Microsoft Comic Chat");
                }
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
