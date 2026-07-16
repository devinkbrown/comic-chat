#include "comicchat/avatar_assets.hpp"

#include <charconv>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

auto parse_double(const std::string_view value) -> std::optional<double> {
    double result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return result;
}

} // namespace

auto main(const int argc, char** argv) -> int {
    if (argc < 3 || argc > 7) {
        std::cerr << "usage: comicchat-avatar-oracle ASSET OUTPUT.png [ANGLE [INTENSITY [WIDTH HEIGHT]]]\n"
                     "       comicchat-avatar-oracle ASSET OUTPUT_DIR --sequence [WIDTH HEIGHT]\n";
        return 2;
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
    if (!angle || !intensity || !width || !height) return 2;
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
                {*selection, static_cast<std::int32_t>(*width), static_cast<std::int32_t>(*height), false, false});
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
        {*selection, static_cast<std::int32_t>(*width), static_cast<std::int32_t>(*height), false, false});
    if (!rendered || !comicchat::write_avatar_png(*rendered, argv[2])) {
        std::cerr << "render failed\n";
        return 1;
    }
    return 0;
}
