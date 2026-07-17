#include "comicchat/avatar_assets.hpp"

#include <algorithm>
#include <charconv>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <locale>
#include <sstream>
#include <string_view>

namespace {

auto parse_double(const std::string_view value) -> std::optional<double> {
    // FreeBSD's current libc++ deliberately omits floating-point from_chars.
    // A classic-locale stream keeps this diagnostic tool portable and avoids
    // accepting locale-specific decimal separators or trailing bytes.
    std::istringstream input{std::string{value}};
    input.imbue(std::locale::classic());
    double result{};
    input >> result;
    if (!input || !input.eof() || !std::isfinite(result)) return std::nullopt;
    return result;
}

auto parse_size(const std::string_view value) -> std::optional<std::size_t> {
    std::size_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return result;
}

auto parse_mode(const std::string_view value) -> std::optional<comicchat::AvatarRenderMode> {
    if (value == "legacy_exact") return comicchat::AvatarRenderMode::legacy_exact;
    if (value == "modern_remaster") return comicchat::AvatarRenderMode::modern_remaster;
    return std::nullopt;
}

auto dark_pixel(const std::uint32_t pixel) noexcept -> bool {
    const auto red = (pixel >> 16U) & 0xffU;
    const auto green = (pixel >> 8U) & 0xffU;
    const auto blue = pixel & 0xffU;
    return red * 2126U + green * 7152U + blue * 722U < 128U * 10'000U;
}

struct Moments final {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};
    double centroid_x{};
    double centroid_y{};
    std::size_t pixels{};
};

auto moments(const comicchat::AvatarBitmap& bitmap) -> Moments {
    auto result = Moments{bitmap.width, bitmap.height, -1, -1, 0.0, 0.0, 0};
    for (std::int32_t y = 0; y < bitmap.height; ++y) {
        for (std::int32_t x = 0; x < bitmap.width; ++x) {
            const auto pixel = bitmap.pixels[static_cast<std::size_t>(y) * bitmap.width + x];
            if (!dark_pixel(pixel)) continue;
            result.left = std::min(result.left, x);
            result.top = std::min(result.top, y);
            result.right = std::max(result.right, x);
            result.bottom = std::max(result.bottom, y);
            result.centroid_x += x;
            result.centroid_y += y;
            ++result.pixels;
        }
    }
    if (result.pixels != 0) {
        result.centroid_x /= static_cast<double>(result.pixels);
        result.centroid_y /= static_cast<double>(result.pixels);
    }
    return result;
}

auto silhouette(const comicchat::AvatarBitmap& bitmap) -> std::array<std::uint16_t, 16> {
    std::array<std::uint16_t, 16> result{};
    for (std::int32_t y = 0; y < bitmap.height; ++y) {
        for (std::int32_t x = 0; x < bitmap.width; ++x) {
            const auto pixel = bitmap.pixels[static_cast<std::size_t>(y) * bitmap.width + x];
            if (dark_pixel(pixel))
                result[static_cast<std::size_t>(y) * result.size() / static_cast<std::size_t>(bitmap.height)] |=
                    static_cast<std::uint16_t>(1U <<
                        (static_cast<std::size_t>(x) * 16U / static_cast<std::size_t>(bitmap.width)));
        }
    }
    return result;
}

void print_fidelity(const comicchat::AvatarBitmap& legacy, const comicchat::AvatarBitmap& modern) {
    const auto legacy_moments = moments(legacy);
    const auto modern_moments = moments(modern);
    const auto legacy_silhouette = silhouette(legacy);
    const auto modern_silhouette = silhouette(modern);
    std::size_t hamming{};
    for (std::size_t row = 0; row < legacy_silhouette.size(); ++row)
        hamming += static_cast<std::size_t>(
            std::popcount<std::uint16_t>(legacy_silhouette[row] ^ modern_silhouette[row]));
    std::size_t threshold_difference{};
    std::size_t soft_ink{};
    for (std::size_t index = 0; index < legacy.pixels.size(); ++index) {
        threshold_difference += static_cast<std::size_t>(dark_pixel(legacy.pixels[index]) !=
            dark_pixel(modern.pixels[index]));
        const auto pixel = modern.pixels[index];
        const auto red = (pixel >> 16U) & 0xffU;
        const auto green = (pixel >> 8U) & 0xffU;
        const auto blue = pixel & 0xffU;
        const auto spread = std::max({red, green, blue}) - std::min({red, green, blue});
        soft_ink += static_cast<std::size_t>(spread <= 8U && red > 8U && red < 247U);
    }
    std::cout << "{\"width\":" << legacy.width << ",\"height\":" << legacy.height
              << ",\"silhouette_hamming_16x16\":" << hamming
              << ",\"threshold_difference_ratio\":"
              << static_cast<double>(threshold_difference) / static_cast<double>(legacy.pixels.size())
              << ",\"bbox_delta\":[" << modern_moments.left - legacy_moments.left << ','
              << modern_moments.top - legacy_moments.top << ','
              << modern_moments.right - legacy_moments.right << ','
              << modern_moments.bottom - legacy_moments.bottom << "]"
              << ",\"centroid_delta\":[" << modern_moments.centroid_x - legacy_moments.centroid_x << ','
              << modern_moments.centroid_y - legacy_moments.centroid_y << "]"
              << ",\"modern_soft_ink_pixels\":" << soft_ink << "}\n";
}

} // namespace

auto main(const int argc, char** argv) -> int {
    const auto compare_modes = argc >= 3 && std::string_view{argv[2]} == "--compare-modes";
    const auto benchmark = argc >= 3 && std::string_view{argv[2]} == "--benchmark";
    if ((compare_modes && argc != 5) || (benchmark && argc != 7) ||
        (!compare_modes && !benchmark && (argc < 3 || argc > 8))) {
        std::cerr << "usage: comicchat-avatar-oracle ASSET OUTPUT.png [ANGLE [INTENSITY [WIDTH HEIGHT [MODE]]]]\n"
                     "       comicchat-avatar-oracle ASSET OUTPUT_DIR --sequence [WIDTH HEIGHT [MODE]]\n"
                     "       comicchat-avatar-oracle ASSET --compare-modes WIDTH HEIGHT\n"
                     "       comicchat-avatar-oracle ASSET --benchmark WIDTH HEIGHT MODE ITERATIONS\n"
                     "       MODE is legacy_exact (default) or modern_remaster\n";
        return 2;
    }
    if (compare_modes) {
        const auto width = parse_double(argv[3]);
        const auto height = parse_double(argv[4]);
        if (!width || !height) return 2;
        const auto asset = comicchat::load_avatar_asset(argv[1]);
        if (!asset) return 1;
        const auto selection = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
        if (!selection) return 1;
        const auto legacy = comicchat::render_avatar(*asset,
            {*selection, static_cast<std::int32_t>(*width), static_cast<std::int32_t>(*height), false, false,
                comicchat::AvatarRenderMode::legacy_exact});
        const auto modern = comicchat::render_avatar(*asset,
            {*selection, static_cast<std::int32_t>(*width), static_cast<std::int32_t>(*height), false, false,
                comicchat::AvatarRenderMode::modern_remaster});
        if (!legacy || !modern) return 1;
        print_fidelity(*legacy, *modern);
        return 0;
    }
    if (benchmark) {
        const auto width = parse_double(argv[3]);
        const auto height = parse_double(argv[4]);
        const auto mode = parse_mode(argv[5]);
        const auto iterations = parse_size(argv[6]);
        if (!width || !height || !mode || !iterations || *iterations == 0 || *iterations > 1000) return 2;
        const auto asset = comicchat::load_avatar_asset(argv[1]);
        if (!asset) return 1;
        const auto selection = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
        if (!selection) return 1;
        const auto request = comicchat::AvatarRenderRequest{*selection, static_cast<std::int32_t>(*width),
            static_cast<std::int32_t>(*height), false, false, *mode};
        if (!comicchat::render_avatar(*asset, request)) return 1; // warm allocator and instruction cache
        std::uint64_t checksum{};
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t iteration = 0; iteration < *iterations; ++iteration) {
            const auto rendered = comicchat::render_avatar(*asset, request);
            if (!rendered) return 1;
            checksum ^= rendered->pixels[(iteration * 7919U) % rendered->pixels.size()];
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        const auto average = elapsed / static_cast<double>(*iterations);
        const auto megapixels = static_cast<double>(*width * *height) / 1'000'000.0;
        std::cout << "{\"width\":" << static_cast<std::int32_t>(*width)
                  << ",\"height\":" << static_cast<std::int32_t>(*height)
                  << ",\"iterations\":" << *iterations << ",\"total_ms\":" << elapsed
                  << ",\"average_ms\":" << average << ",\"frames_per_second\":" << 1000.0 / average
                  << ",\"megapixels_per_second\":" << megapixels * 1000.0 / average
                  << ",\"checksum\":" << checksum << "}\n";
        return 0;
    }
    const auto sequence = argc >= 4 && std::string_view{argv[3]} == "--sequence";
    const auto angle = !sequence && argc >= 4 ? parse_double(argv[3]) : std::optional<double>{0.0};
    const auto intensity = !sequence && argc >= 5 ? parse_double(argv[4]) : std::optional<double>{0.0};
    const auto width = sequence
        ? (argc >= 5 ? parse_double(argv[4]) : std::optional<double>{150.0})
        : (argc >= 6 ? parse_double(argv[5]) : std::optional<double>{149.0});
    const auto height = sequence
        ? (argc >= 6 ? parse_double(argv[5]) : std::optional<double>{133.0})
        : (argc >= 7 ? parse_double(argv[6]) : std::optional<double>{133.0});
    const auto mode = sequence
        ? (argc >= 7 ? parse_mode(argv[6]) : std::optional{comicchat::AvatarRenderMode::legacy_exact})
        : (argc >= 8 ? parse_mode(argv[7]) : std::optional{comicchat::AvatarRenderMode::legacy_exact});
    if (!angle || !intensity || !width || !height || !mode) return 2;
    const auto asset = comicchat::load_avatar_asset(argv[1]);
    if (!asset) {
        std::cerr << "asset load error " << static_cast<int>(asset.error()) << '\n';
        return 1;
    }
    if (std::string_view{argv[2]} == "--inspect") {
        const auto print = [](const std::string_view label,
            const std::vector<comicchat::AvatarComponent>& components) {
            for (std::size_t index = 0; index < components.size(); ++index)
                std::cout << label << '[' << index << "] pose=" << components[index].pose_id
                          << " emotion=" << components[index].emotion_index
                          << " intensity=" << static_cast<unsigned>(components[index].intensity) << '\n';
        };
        print("body", asset->bodies);
        print("face", asset->faces);
        print("torso", asset->torsos);
        return 0;
    }
    if (sequence) {
        constexpr std::array<std::string_view, 9> names{
            "neutral", "east", "southeast", "south", "southwest", "west", "northwest", "north", "northeast"};
        constexpr std::array<comicchat::AvatarExpression, 9> expressions{{
            {0.0, 0.0}, {0.0, 1.0}, {0.7853981633974483, 1.0}, {1.5707963267948966, 1.0},
            {2.356194490192345, 1.0}, {3.141592653589793, 1.0}, {-2.356194490192345, 1.0},
            {-1.5707963267948966, 1.0}, {-0.7853981633974483, 1.0},
        }};
        const auto output = std::filesystem::path{argv[2]};
        std::error_code error;
        std::filesystem::create_directories(output, error);
        if (error) return 1;
        std::optional<comicchat::AvatarSelection> previous;
        for (std::size_t index = 0; index < expressions.size(); ++index) {
            const auto selection = comicchat::select_avatar_expression(*asset, expressions[index], previous);
            if (!selection) return 1;
            previous = *selection;
            const auto rendered = comicchat::render_avatar(*asset,
                {*selection, static_cast<std::int32_t>(*width), static_cast<std::int32_t>(*height),
                    false, false, *mode});
            const auto filename = output / (std::to_string(index) + "-" + std::string{names[index]} + ".png");
            if (!rendered || !comicchat::write_avatar_png(*rendered, filename)) return 1;
        }
        return 0;
    }
    const auto selection = comicchat::select_avatar_expression(*asset, {*angle, *intensity});
    if (!selection) {
        std::cerr << "pose selection error " << static_cast<int>(selection.error()) << '\n';
        return 1;
    }
    const auto rendered = comicchat::render_avatar(*asset,
        {*selection, static_cast<std::int32_t>(*width), static_cast<std::int32_t>(*height),
            false, false, *mode});
    if (!rendered || !comicchat::write_avatar_png(*rendered, argv[2])) {
        std::cerr << "render failed\n";
        return 1;
    }
    return 0;
}
