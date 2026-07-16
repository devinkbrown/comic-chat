#include "comicchat/render.hpp"
#include "comicchat/text.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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

struct Arguments final {
    std::optional<std::string> font;
    std::optional<std::string> png;
    std::uint64_t frames{};
};

auto parse_arguments(const int argc, char** argv) -> std::optional<Arguments> {
    Arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if ((argument == "--font" || argument == "--png" || argument == "--frames") && index + 1 >= argc) {
            return std::nullopt;
        }
        if (argument == "--font") result.font = argv[++index];
        else if (argument == "--png") result.png = argv[++index];
        else if (argument == "--frames") {
            const std::string_view value{argv[++index]};
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result.frames);
            if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
        } else if (argument == "--help") {
            std::cout << "comic-chat [--font FILE] [--png FILE] [--frames COUNT]\n";
            return Arguments{std::string{}, std::string{}, 0};
        } else {
            return std::nullopt;
        }
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

} // namespace

auto main(const int argc, char** argv) -> int {
    try {
        const auto arguments = parse_arguments(argc, argv);
        if (!arguments) {
            std::cerr << "usage: comic-chat [--font FILE] [--png FILE] [--frames COUNT]\n";
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

        Sdl sdl;
        SDL_Window* raw_window{};
        SDL_Renderer* raw_renderer{};
        if (!SDL_CreateWindowAndRenderer("Microsoft Comic Chat — Native Port", 760, 760,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                        &raw_window, &raw_renderer)) {
            throw std::runtime_error{SDL_GetError()};
        }
        std::unique_ptr<SDL_Window, WindowDeleter> window{raw_window};
        std::unique_ptr<SDL_Renderer, RendererDeleter> renderer{raw_renderer};
        std::unique_ptr<SDL_Texture, TextureDeleter> texture;
        std::unique_ptr<comicchat::Canvas> canvas;

        const auto rebuild = [&] {
            int width{};
            int height{};
            if (!SDL_GetWindowSizeInPixels(window.get(), &width, &height)) throw std::runtime_error{SDL_GetError()};
            width = std::max(width, 1);
            height = std::max(height, 1);
            canvas = std::make_unique<comicchat::Canvas>(width, height);
            canvas->clear({1.0, 1.0, 1.0, 1.0});
            canvas->render_title_panel(model(), **text);
            texture.reset(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888,
                                            SDL_TEXTUREACCESS_STREAMING, width, height));
            if (!texture) throw std::runtime_error{SDL_GetError()};
            if (!SDL_UpdateTexture(texture.get(), nullptr, canvas->pixels().data(), width * 4)) {
                throw std::runtime_error{SDL_GetError()};
            }
        };
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
