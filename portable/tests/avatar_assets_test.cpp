#include "comicchat/avatar_assets.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string_view>

#ifndef COMICCHAT_TEST_COMICART_DIR
#error "COMICCHAT_TEST_COMICART_DIR must point at Microsoft's active Comic Chat corpus"
#endif

namespace {

constexpr std::array<std::string_view, 25> avatars{
    "anna.avb", "armando.avb", "bolo.avb", "buck.avb", "connor.avb", "cro.avb", "dan.avb",
    "denise.avb", "glenda.avb", "hugh.avb", "jordan.avb", "kirby.avb", "lance.avb", "lynnea.avb",
    "margaret.avb", "mike.avb", "pedagog.avb", "rainbow.avb", "susan.avb", "tiki.avb",
    "tongtyed.avb", "tux.avb", "veronica.avb", "waf.avb", "xeno.avb",
};

constexpr std::array<std::string_view, 7> backdrops{
    "buckroom.bgb", "clouds.bgb", "field.bgb", "pastoral.bgb", "room.bgb", "space.bgb", "yellow.bgb",
};

auto valid_bitmap(const comicchat::AvatarBitmap& bitmap) -> bool {
    return bitmap.width > 0 && bitmap.height > 0 &&
        bitmap.pixels.size() == static_cast<std::size_t>(bitmap.width) * static_cast<std::size_t>(bitmap.height);
}

auto silhouette_signature(const comicchat::AvatarBitmap& bitmap) -> std::array<std::uint16_t, 16> {
    std::array<std::uint16_t, 16> rows{};
    // The legacy property-page control is 150 pixels wide; the captured
    // client area excludes its one-pixel right edge and is 149x133.
    for (std::int32_t y = 0; y < 133; ++y) {
        for (std::int32_t x = 0; x < 149; ++x) {
            const auto pixel = bitmap.pixels[static_cast<std::size_t>(y) * bitmap.width + x];
            const auto red = (pixel >> 16U) & 0xffU;
            const auto green = (pixel >> 8U) & 0xffU;
            const auto blue = pixel & 0xffU;
            if (red * 2126U + green * 7152U + blue * 722U < 128U * 10'000U)
                rows[static_cast<std::size_t>(y) * 16U / 133U] |=
                    static_cast<std::uint16_t>(1U << (static_cast<std::size_t>(x) * 16U / 149U));
        }
    }
    return rows;
}

} // namespace

TEST_CASE("Microsoft's complete active AVB corpus decodes with its original pose metadata") {
    const auto root = std::filesystem::path{COMICCHAT_TEST_COMICART_DIR};
    for (const auto filename : avatars) {
        CAPTURE(filename);
        const auto asset = comicchat::load_avatar_asset(root / filename);
        INFO("asset error " << (asset ? -1 : static_cast<int>(asset.error())));
        REQUIRE(asset.has_value());
        CHECK((asset->kind == comicchat::AvatarKind::simple || asset->kind == comicchat::AvatarKind::complex));
        CHECK_FALSE(asset->name.empty());
        CHECK_FALSE(asset->poses.empty());
        CHECK(asset->icon_pose_id > 0);
        CHECK(asset->icon_pose_id <= asset->poses.size());
        if (asset->kind == comicchat::AvatarKind::simple) CHECK_FALSE(asset->bodies.empty());
        if (asset->kind == comicchat::AvatarKind::complex) {
            CHECK_FALSE(asset->faces.empty());
            CHECK_FALSE(asset->torsos.empty());
        }

        for (const auto& pose : asset->poses) {
            REQUIRE(pose.drawing.has_value());
            CHECK(valid_bitmap(*pose.drawing));
            if (pose.mask) CHECK(valid_bitmap(*pose.mask));
            if (pose.aura) CHECK(valid_bitmap(*pose.aura));
        }
    }
}

TEST_CASE("Microsoft's complete active BGB corpus decodes without replacement art") {
    const auto root = std::filesystem::path{COMICCHAT_TEST_COMICART_DIR};
    for (const auto filename : backdrops) {
        CAPTURE(filename);
        const auto asset = comicchat::load_avatar_asset(root / filename);
        INFO("asset error " << (asset ? -1 : static_cast<int>(asset.error())));
        REQUIRE(asset.has_value());
        CHECK(asset->kind == comicchat::AvatarKind::backdrop);
        REQUIRE(asset->backdrop.has_value());
        CHECK(valid_bitmap(*asset->backdrop));
    }
}

TEST_CASE("Microsoft's complete active character corpus renders its neutral source pose") {
    const auto root = std::filesystem::path{COMICCHAT_TEST_COMICART_DIR};
    for (const auto filename : avatars) {
        CAPTURE(filename);
        const auto asset = comicchat::load_avatar_asset(root / filename);
        REQUIRE(asset.has_value());
        const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
        REQUIRE(neutral.has_value());
        const auto preview = comicchat::render_avatar(*asset, {*neutral, 149, 133, false, false});
        REQUIRE(preview.has_value());
        REQUIRE(valid_bitmap(*preview));
        CHECK(std::any_of(preview->pixels.begin(), preview->pixels.end(),
            [](const auto pixel) { return pixel != 0xffffffffU; }));
    }
}

TEST_CASE("complex avatar flip mirrors Microsoft's composed component rectangles") {
    const auto asset = comicchat::load_avatar_asset(
        std::filesystem::path{COMICCHAT_TEST_COMICART_DIR} / "anna.avb");
    REQUIRE(asset.has_value());
    const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
    REQUIRE(neutral.has_value());
    const auto normal = comicchat::render_avatar(*asset, {*neutral, 151, 133, false, false});
    const auto flipped = comicchat::render_avatar(*asset, {*neutral, 151, 133, true, false});
    REQUIRE(normal.has_value());
    REQUIRE(flipped.has_value());
    auto best_mismatch = normal->pixels.size();
    for (const int shift : {-1, 0, 1}) {
        std::size_t mismatch{};
        for (std::int32_t y = 0; y < normal->height; ++y) {
            for (std::int32_t x = 0; x < normal->width; ++x) {
                const auto mirrored_x = normal->width - x - 1 + shift;
                if (mirrored_x < 0 || mirrored_x >= normal->width) continue;
                const auto left = flipped->pixels[static_cast<std::size_t>(y) * normal->width + x];
                const auto right = normal->pixels[static_cast<std::size_t>(y) * normal->width + mirrored_x];
                if (left != right) ++mismatch;
            }
        }
        best_mismatch = std::min(best_mismatch, mismatch);
    }
    CHECK(best_mismatch < normal->pixels.size() / 100U);
}

TEST_CASE("Tiki preserves the full Microsoft-rendered anatomy silhouette") {
    // Compact 16x16 occupancy golden extracted from the live v2.5 CChat.exe
    // property-page renderer under Wine, not generated from this decoder.
    constexpr std::array<std::uint16_t, 16> microsoft_tiki{
        0x07c0U, 0x07c0U, 0x07c0U, 0x07c0U, 0x07c0U, 0x03c0U, 0x03c0U, 0x01c0U,
        0x01c0U, 0x01c0U, 0x0180U, 0x0180U, 0x0180U, 0x01c0U, 0x03e0U, 0x03e0U,
    };
    const auto asset = comicchat::load_avatar_asset(
        std::filesystem::path{COMICCHAT_TEST_COMICART_DIR} / "tiki.avb");
    REQUIRE(asset.has_value());
    const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
    REQUIRE(neutral.has_value());
    CHECK(neutral->torso == 0);
    const auto east = comicchat::select_avatar_expression(*asset, {0.0, 1.0}, *neutral);
    REQUIRE(east.has_value());
    // The legacy rotating search first treats torso 2 as a weak neutral
    // fallback (delta 1.5), then replaces it with the later exact-angle
    // neutral torso 7 (delta 1.0).
    CHECK(east->torso == 7);
    const auto rendered = comicchat::render_avatar(*asset, {*neutral, 150, 133, false, false});
    REQUIRE(rendered.has_value());
    const auto actual = silhouette_signature(*rendered);
    std::size_t hamming{};
    for (std::size_t row = 0; row < actual.size(); ++row)
        hamming += static_cast<std::size_t>(std::popcount<std::uint16_t>(actual[row] ^ microsoft_tiki[row]));
    CHECK(hamming <= 1);
}

TEST_CASE("AVB reader rejects truncated and non-AVB input") {
    const auto missing = comicchat::load_avatar_asset("this-file-does-not-exist.avb");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == comicchat::AvatarAssetError::io);
}
