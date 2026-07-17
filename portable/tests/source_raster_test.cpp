#include "comicchat/source_raster.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <string_view>

#ifndef COMICCHAT_TEST_SOURCE_RASTER_DIR
#error "COMICCHAT_TEST_SOURCE_RASTER_DIR must name the released Microsoft res directory"
#endif

namespace {

const std::filesystem::path source_root{COMICCHAT_TEST_SOURCE_RASTER_DIR};

auto alpha_count(const comicchat::RasterImage& image) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(image.argb, [](const std::uint32_t pixel) {
        return (pixel >> 24U) != 0U;
    }));
}

auto rgb_count(const comicchat::RasterImage& image, const std::uint32_t rgb) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(image.argb, [rgb](const std::uint32_t pixel) {
        return (pixel & 0x00ff'ffffU) == rgb;
    }));
}

auto cell_alpha_count(const comicchat::RasterImage& image, const comicchat::RasterCell& cell) -> std::size_t {
    std::size_t result{};
    for (std::uint32_t y = cell.y; y < static_cast<std::uint32_t>(cell.y) + cell.height; ++y) {
        for (std::uint32_t x = cell.x; x < static_cast<std::uint32_t>(cell.x) + cell.width; ++x) {
            if ((image.argb[static_cast<std::size_t>(y) * image.width + x] >> 24U) != 0U) ++result;
        }
    }
    return result;
}

} // namespace

TEST_CASE("source catalog preserves Microsoft's icon and strip semantics") {
    const auto icons = comicchat::source_icon_catalog();
    const auto strips = comicchat::source_strip_catalog();
    REQUIRE(icons.size() == 11);
    REQUIRE(strips.size() == 12);

    CHECK(comicchat::source_icon_spec(comicchat::SourceIcon::application).file_name == "chat.ico");
    CHECK(comicchat::source_icon_spec(comicchat::SourceIcon::notification).file_name == "notif.ico");
    CHECK(comicchat::source_icon_spec(comicchat::SourceIcon::connect_server).file_name == "tosrv.ico");
    CHECK(comicchat::source_icon_spec(comicchat::SourceIcon::connect_network).file_name == "tonet.ico");

    const auto& say = comicchat::source_strip_spec(comicchat::SourceStrip::say_toolbar);
    REQUIRE(say.width == 118);
    REQUIRE(say.height == 17);
    REQUIRE(say.cells.size() == 7);
    CHECK(say.cells[0] == comicchat::RasterCell{0, 0, 17, 17, "say"});
    CHECK(say.cells[5] == comicchat::RasterCell{85, 0, 17, 17, "sound"});
    CHECK(say.cells[6] == comicchat::RasterCell{102, 0, 16, 17, "whisper_sound"});

    const auto& user = comicchat::source_strip_spec(comicchat::SourceStrip::user_toolbar);
    REQUIRE(user.cells.size() == 7);
    CHECK(user.cells.back().semantic == "netmeeting");
    CHECK(user.cells.back().semantic != "send_file");
}

TEST_CASE("ICO decoder reproduces the released application icon mask and frame order") {
    const auto icon = comicchat::load_source_icon(
        source_root, comicchat::SourceIcon::application, 32);
    INFO("icon error " << (icon ? -1 : static_cast<int>(icon.error())));
    REQUIRE(icon.has_value());
    CHECK(icon->width == 32);
    CHECK(icon->height == 32);
    CHECK(icon->argb.size() == 32U * 32U);
    CHECK(alpha_count(*icon) == 849);

    std::uint32_t min_x = icon->width;
    std::uint32_t min_y = icon->height;
    std::uint32_t max_x{};
    std::uint32_t max_y{};
    for (std::uint32_t y = 0; y < icon->height; ++y) {
        for (std::uint32_t x = 0; x < icon->width; ++x) {
            if ((icon->argb[static_cast<std::size_t>(y) * icon->width + x] >> 24U) == 0U) continue;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    CHECK(std::array{min_x, min_y, max_x, max_y} == std::array<std::uint32_t, 4>{1, 0, 31, 31});

    const auto small = comicchat::load_source_icon(
        source_root, comicchat::SourceIcon::application, 16);
    REQUIRE(small.has_value());
    CHECK(small->width == 16);
    CHECK(small->height == 16);
    CHECK(alpha_count(*small) == 203);

    struct ExpectedIcon final {
        comicchat::SourceIcon id;
        std::uint32_t width;
        std::size_t opaque;
    };
    constexpr std::array expected_icons{
        ExpectedIcon{comicchat::SourceIcon::application, 32, 849},
        ExpectedIcon{comicchat::SourceIcon::document, 32, 700},
        ExpectedIcon{comicchat::SourceIcon::room, 32, 525},
        ExpectedIcon{comicchat::SourceIcon::ruleset, 32, 1'024},
        ExpectedIcon{comicchat::SourceIcon::avatar, 32, 811},
        ExpectedIcon{comicchat::SourceIcon::background, 32, 817},
        ExpectedIcon{comicchat::SourceIcon::ratings, 32, 486},
        ExpectedIcon{comicchat::SourceIcon::whisper, 32, 608},
        ExpectedIcon{comicchat::SourceIcon::notification, 32, 384},
        ExpectedIcon{comicchat::SourceIcon::connect_server, 16, 223},
        ExpectedIcon{comicchat::SourceIcon::connect_network, 16, 155},
    };
    for (const auto& expected : expected_icons) {
        const auto decoded = comicchat::load_source_icon(source_root, expected.id, expected.width);
        INFO("icon " << comicchat::source_icon_spec(expected.id).file_name << " error "
                     << (decoded ? -1 : static_cast<int>(decoded.error())));
        REQUIRE(decoded.has_value());
        CHECK(decoded->width == expected.width);
        CHECK(decoded->height == expected.width);
        CHECK(alpha_count(*decoded) == expected.opaque);
    }
}

TEST_CASE("BMP decoder preserves exact source dimensions and RLE4 About artwork") {
    for (const auto& spec : comicchat::source_strip_catalog()) {
        const auto image = comicchat::load_source_strip(source_root, spec.id);
        INFO("strip " << spec.file_name << " error "
                      << (image ? -1 : static_cast<int>(image.error())));
        REQUIRE(image.has_value());
        CHECK(image->width == spec.width);
        CHECK(image->height == spec.height);
        for (const auto& cell : spec.cells) {
            CHECK(static_cast<std::uint32_t>(cell.x) + cell.width <= image->width);
            CHECK(static_cast<std::uint32_t>(cell.y) + cell.height <= image->height);
        }
    }

    const auto tiki = comicchat::load_source_strip(source_root, comicchat::SourceStrip::about_tiki);
    REQUIRE(tiki.has_value());
    CHECK(rgb_count(*tiki, 0x00ff'ffffU) == 109'856);
    CHECK(rgb_count(*tiki, 0x0000'0000U) == 7'335);
    CHECK(rgb_count(*tiki, 0x0080'8080U) == 3'230);
    CHECK(rgb_count(*tiki, 0x00c0'c0c0U) == 2'079);

    const auto main = comicchat::load_source_strip(source_root, comicchat::SourceStrip::main_toolbar);
    REQUIRE(main.has_value());
    CHECK(cell_alpha_count(*main, comicchat::source_strip_spec(comicchat::SourceStrip::main_toolbar).cells[0]) == 65);
    CHECK(cell_alpha_count(*main, comicchat::source_strip_spec(comicchat::SourceStrip::main_toolbar).cells[1]) == 70);

    const auto members = comicchat::load_source_strip(source_root, comicchat::SourceStrip::member_status);
    REQUIRE(members.has_value());
    constexpr std::array<std::size_t, 5> member_opaque{185, 93, 82, 142, 127};
    for (std::size_t index = 0; index < member_opaque.size(); ++index) {
        CHECK(cell_alpha_count(
            *members, comicchat::source_strip_spec(comicchat::SourceStrip::member_status).cells[index]) ==
            member_opaque[index]);
    }

    const auto connection = comicchat::load_source_strip(source_root, comicchat::SourceStrip::connection);
    REQUIRE(connection.has_value());
    CHECK(cell_alpha_count(*connection, comicchat::source_strip_spec(comicchat::SourceStrip::connection).cells[0]) == 96);
    CHECK(cell_alpha_count(*connection, comicchat::source_strip_spec(comicchat::SourceStrip::connection).cells[1]) == 94);

    const auto old_new = comicchat::load_source_strip(source_root, comicchat::SourceStrip::old_new);
    REQUIRE(old_new.has_value());
    CHECK(alpha_count(*old_new) == 43);
    for (const auto id : {comicchat::SourceStrip::rule_stopped,
                          comicchat::SourceStrip::rule_inactive,
                          comicchat::SourceStrip::rule_active}) {
        const auto image = comicchat::load_source_strip(source_root, id);
        REQUIRE(image.has_value());
        CHECK(alpha_count(*image) == 16U * 15U);
    }
}

TEST_CASE("source raster decoders reject truncated and unrelated input") {
    constexpr std::array<std::byte, 6> short_ico{
        std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0}, std::byte{1}, std::byte{0},
    };
    constexpr std::array<std::byte, 14> short_bmp{
        std::byte{'B'}, std::byte{'M'}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{14}, std::byte{0},
        std::byte{0}, std::byte{0},
    };
    CHECK_FALSE(comicchat::decode_windows_icon(short_ico, 32).has_value());
    CHECK_FALSE(comicchat::decode_windows_bitmap(short_bmp).has_value());
    CHECK_FALSE(comicchat::decode_windows_icon(short_bmp, 32).has_value());
}
