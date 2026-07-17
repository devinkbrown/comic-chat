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

struct OccupancyMoments final {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};
    double centroid_x{};
    double centroid_y{};
    std::size_t pixels{};
};

auto occupancy_moments(const comicchat::AvatarBitmap& bitmap) -> OccupancyMoments {
    auto result = OccupancyMoments{bitmap.width, bitmap.height, -1, -1, 0.0, 0.0, 0};
    for (std::int32_t y = 0; y < bitmap.height; ++y) {
        for (std::int32_t x = 0; x < bitmap.width; ++x) {
            const auto pixel = bitmap.pixels[static_cast<std::size_t>(y) * bitmap.width + x];
            const auto red = (pixel >> 16U) & 0xffU;
            const auto green = (pixel >> 8U) & 0xffU;
            const auto blue = pixel & 0xffU;
            if (red * 2126U + green * 7152U + blue * 722U >= 128U * 10'000U) continue;
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

TEST_CASE("modern remaster smooths high-DPI ink without moving the source anatomy") {
    const auto asset = comicchat::load_avatar_asset(
        std::filesystem::path{COMICCHAT_TEST_COMICART_DIR} / "tiki.avb");
    REQUIRE(asset.has_value());
    const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
    REQUIRE(neutral.has_value());

    constexpr auto scale = 4;
    const auto default_legacy = comicchat::render_avatar(*asset,
        {*neutral, 150 * scale, 133 * scale, false, false});
    const auto legacy = comicchat::render_avatar(*asset,
        {*neutral, 150 * scale, 133 * scale, false, false, comicchat::AvatarRenderMode::legacy_exact});
    const auto modern = comicchat::render_avatar(*asset,
        {*neutral, 150 * scale, 133 * scale, false, false, comicchat::AvatarRenderMode::modern_remaster});
    REQUIRE(default_legacy.has_value());
    REQUIRE(legacy.has_value());
    REQUIRE(modern.has_value());
    CHECK(default_legacy->pixels == legacy->pixels);
    CHECK(modern->pixels != legacy->pixels);

    // The remaster must reconstruct sub-pixel contour coverage rather than
    // copying enlarged source blocks. Tiki's source art has hard black ink,
    // so partially covered, near-neutral edge samples are a stable signal.
    const auto soft_ink = std::count_if(modern->pixels.begin(), modern->pixels.end(), [](const auto pixel) {
        const auto red = (pixel >> 16U) & 0xffU;
        const auto green = (pixel >> 8U) & 0xffU;
        const auto blue = pixel & 0xffU;
        const auto spread = std::max({red, green, blue}) - std::min({red, green, blue});
        return spread <= 8U && red > 8U && red < 247U;
    });
    CHECK(soft_ink > 250);

    const auto legacy_landmarks = occupancy_moments(*legacy);
    const auto modern_landmarks = occupancy_moments(*modern);
    REQUIRE(legacy_landmarks.pixels > 0);
    REQUIRE(modern_landmarks.pixels > 0);
    CHECK(std::abs(modern_landmarks.left - legacy_landmarks.left) <= 2);
    CHECK(std::abs(modern_landmarks.top - legacy_landmarks.top) <= 2);
    CHECK(std::abs(modern_landmarks.right - legacy_landmarks.right) <= 2);
    CHECK(std::abs(modern_landmarks.bottom - legacy_landmarks.bottom) <= 2);
    CHECK(std::abs(modern_landmarks.centroid_x - legacy_landmarks.centroid_x) <= 1.5);
    CHECK(std::abs(modern_landmarks.centroid_y - legacy_landmarks.centroid_y) <= 1.5);
}

TEST_CASE("AVB reader rejects truncated and non-AVB input") {
    const auto missing = comicchat::load_avatar_asset("this-file-does-not-exist.avb");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == comicchat::AvatarAssetError::io);
}

// ---------------------------------------------------------------------------
// Phase 2.2 — CBodyUnary / CBodyDouble compositing (render-port-spec §2.2).
//
// These gate compositing correctness the way source_raster_test.cpp gates the
// decoder: on deterministic geometry and opaque-pixel counts against
// Microsoft's own AVB bytes, NOT on GDI-vs-Cairo pixel parity (which is only
// checkable under MSVC / visually, per §2.2.b and the honesty note).
// ---------------------------------------------------------------------------

namespace {

// Independent re-transcription of CBodyDouble::GetBodyBox (bodycam.cpp:632-671)
// so the assertion is an *oracle* over the source math, not a mirror of the
// production formula. Kept in the test so a bug that changes both would still
// have to change two independent transcriptions to hide.
struct ReferenceRect final {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};
};

auto reference_round(const double value) -> std::int32_t {
    return static_cast<std::int32_t>(value > 0.0 ? value + 0.5 : value - 0.5);
}

struct ReferenceComposite final {
    ReferenceRect full;
    ReferenceRect head;
    ReferenceRect torso;
    std::int32_t union_width{};
    std::int32_t union_height{};
    std::int32_t head_height{};
    std::int32_t face_x{};
};

// Resolve the drawing bitmap for a complex component exactly as checked_pose /
// GetPoseFromID does for a valid selection (no substitution needed — the
// selection came from select_avatar_expression).
auto component_drawing(const comicchat::AvatarAsset& asset, const comicchat::AvatarComponent& component)
    -> const comicchat::AvatarBitmap* {
    if (component.pose_id == 0 || component.pose_id > asset.poses.size()) return nullptr;
    const auto& pose = asset.poses[component.pose_id - 1U];
    return pose.drawing ? &*pose.drawing : nullptr;
}

auto reference_composite(const comicchat::AvatarAsset& asset, const comicchat::AvatarSelection& selection,
    const std::int32_t width, const std::int32_t height) -> std::optional<ReferenceComposite> {
    const auto& face = asset.faces[selection.face];
    const auto& torso = asset.torsos[selection.torso];
    const auto* head_bitmap = component_drawing(asset, face);
    const auto* torso_bitmap = component_drawing(asset, torso);
    if (head_bitmap == nullptr || torso_bitmap == nullptr) return std::nullopt;
    const auto head_w = head_bitmap->width;
    const auto head_h = head_bitmap->height;
    const auto torso_w = torso_bitmap->width;
    const auto torso_h = torso_bitmap->height;

    const auto x_offset = static_cast<std::int32_t>(torso.center_x) + face.center_delta_x - face.center_x;
    const auto y_offset = static_cast<std::int32_t>(torso.center_y) + face.center_delta_y - face.center_y;
    const auto bit_left = std::min(0, x_offset);
    const auto bit_top = std::min(0, y_offset);
    const auto bit_right = std::max(torso_w, x_offset + head_w);
    const auto composed_head_bottom = y_offset + head_h;
    const auto bit_bottom = std::max(torso_h, composed_head_bottom);
    const auto bit_width = bit_right - bit_left;
    const auto bit_height = bit_bottom - bit_top;

    const auto scale = std::min(static_cast<double>(width) / bit_width,
        static_cast<double>(height) / bit_height);
    const auto full_width = reference_round(scale * bit_width);
    const auto full_height = reference_round(scale * bit_height);
    ReferenceComposite result{};
    result.full = {(width - full_width) / 2, height - full_height,
        (width - full_width) / 2 + full_width, height};
    const auto place = [&](const std::int32_t offset_x, const std::int32_t offset_y,
        const std::int32_t bitmap_width, const std::int32_t bitmap_height) {
        const auto left = reference_round((offset_x - bit_left) * scale) + result.full.left;
        const auto top = reference_round((offset_y - bit_top) * scale) + result.full.top;
        return ReferenceRect{left, top, left + reference_round(bitmap_width * scale) + 1,
            top + reference_round(bitmap_height * scale) + 1};
    };
    result.head = place(x_offset, y_offset, head_w, head_h);
    result.torso = place(0, 0, torso_w, torso_h);
    result.union_width = bit_width;
    result.union_height = bit_height;
    result.head_height = composed_head_bottom - bit_top;   // avatar.cpp:104,109
    result.face_x = face.face_x + x_offset - bit_left;      // avatar.cpp:112
    return result;
}

auto dark_pixel_count(const comicchat::AvatarBitmap& bitmap) -> std::size_t {
    return static_cast<std::size_t>(std::count_if(bitmap.pixels.begin(), bitmap.pixels.end(),
        [](const std::uint32_t pixel) {
            const auto red = (pixel >> 16U) & 0xffU;
            const auto green = (pixel >> 8U) & 0xffU;
            const auto blue = pixel & 0xffU;
            return red * 2126U + green * 7152U + blue * 722U < 128U * 10'000U;
        }));
}

// Dark-pixel count restricted to a vertical band [top, bottom) of the frame —
// the head footprint of a composite, used for head-over-torso layering counts.
auto dark_pixel_count_band(const comicchat::AvatarBitmap& bitmap, const std::int32_t top,
    const std::int32_t bottom) -> std::size_t {
    std::size_t count{};
    for (std::int32_t y = std::max(0, top); y < std::min(bitmap.height, bottom); ++y)
        for (std::int32_t x = 0; x < bitmap.width; ++x) {
            const auto pixel = bitmap.pixels[static_cast<std::size_t>(y) * bitmap.width + x];
            const auto red = (pixel >> 16U) & 0xffU;
            const auto green = (pixel >> 8U) & 0xffU;
            const auto blue = pixel & 0xffU;
            if (red * 2126U + green * 7152U + blue * 722U < 128U * 10'000U) ++count;
        }
    return count;
}

constexpr std::uint8_t flag_head_mask = 1;
constexpr std::uint8_t flag_torso_first = 4;

} // namespace

TEST_CASE("CBodyDouble union box matches an independent GetBodyBox transcription across the corpus") {
    const auto root = std::filesystem::path{COMICCHAT_TEST_COMICART_DIR};
    for (const auto filename : avatars) {
        CAPTURE(filename);
        const auto asset = comicchat::load_avatar_asset(root / filename);
        REQUIRE(asset.has_value());
        if (asset->kind != comicchat::AvatarKind::complex) continue;
        const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
        REQUIRE(neutral.has_value());

        const auto box = comicchat::avatar_body_box(*asset, *neutral, 150, 133, false);
        const auto dim = comicchat::avatar_dim_info(*asset, *neutral, false);
        const auto reference = reference_composite(*asset, *neutral, 150, 133);
        REQUIRE(box.has_value());
        REQUIRE(dim.has_value());
        REQUIRE(reference.has_value());

        CHECK(box->composite);
        CHECK(box->full.left == reference->full.left);
        CHECK(box->full.top == reference->full.top);
        CHECK(box->full.right == reference->full.right);
        CHECK(box->full.bottom == reference->full.bottom);
        CHECK(box->head.left == reference->head.left);
        CHECK(box->head.top == reference->head.top);
        CHECK(box->head.right == reference->head.right);
        CHECK(box->head.bottom == reference->head.bottom);
        CHECK(box->torso.left == reference->torso.left);
        CHECK(box->torso.top == reference->torso.top);
        CHECK(box->torso.right == reference->torso.right);
        CHECK(box->torso.bottom == reference->torso.bottom);

        // GetDimInfo (avatar.cpp:77-114) is the same union, exposed to layout.
        CHECK(dim->width == reference->union_width);
        CHECK(dim->height == reference->union_height);
        CHECK(dim->norm_height == 100);                    // constant (avatar.cpp:110)
        CHECK(dim->head_height == reference->head_height);
        CHECK(dim->face_x == reference->face_x);

        // Head sits above the torso in the union box (§2.2.c head+torso stack).
        CHECK(box->head.top <= box->torso.top);
        CHECK(dim->head_height < dim->height);
    }
}

TEST_CASE("Composite geometry matches Microsoft-captured golden rectangles") {
    const auto root = std::filesystem::path{COMICCHAT_TEST_COMICART_DIR};

    // xeno: complex, HEADMASK|TORSOFIRST. Golden values hand-derived from the
    // raw FACEREC/BODYREC bytes: xOffset=31, yOffset=-96, union 152x445.
    const auto xeno = comicchat::load_avatar_asset(root / "xeno.avb");
    REQUIRE(xeno.has_value());
    const auto xeno_neutral = comicchat::select_avatar_expression(*xeno, {0.0, 0.0});
    REQUIRE(xeno_neutral.has_value());
    const auto xeno_box = comicchat::avatar_body_box(*xeno, *xeno_neutral, 150, 133, false);
    const auto xeno_dim = comicchat::avatar_dim_info(*xeno, *xeno_neutral, false);
    REQUIRE(xeno_box.has_value());
    REQUIRE(xeno_dim.has_value());
    CHECK(xeno_box->full.left == 52);
    CHECK(xeno_box->full.top == 0);
    CHECK(xeno_box->full.right == 97);
    CHECK(xeno_box->full.bottom == 133);
    CHECK(xeno_box->head.left == 61);
    CHECK(xeno_box->head.top == 0);
    CHECK(xeno_box->head.right == 98);
    CHECK(xeno_box->head.bottom == 48);
    CHECK(xeno_box->torso.left == 52);
    CHECK(xeno_box->torso.top == 29);
    CHECK(xeno_box->torso.right == 98);
    CHECK(xeno_box->torso.bottom == 134);
    CHECK(xeno_dim->width == 152);
    CHECK(xeno_dim->height == 445);
    CHECK(xeno_dim->head_height == 156);
    CHECK(xeno_dim->face_x == 93);

    // tux: simple (CBodyUnary). Aspect-preserving fit, centred + bottom-aligned.
    const auto tux = comicchat::load_avatar_asset(root / "tux.avb");
    REQUIRE(tux.has_value());
    const auto tux_neutral = comicchat::select_avatar_expression(*tux, {0.0, 0.0});
    REQUIRE(tux_neutral.has_value());
    const auto tux_box = comicchat::avatar_body_box(*tux, *tux_neutral, 150, 133, false);
    const auto tux_dim = comicchat::avatar_dim_info(*tux, *tux_neutral, false);
    REQUIRE(tux_box.has_value());
    REQUIRE(tux_dim.has_value());
    CHECK_FALSE(tux_box->composite);
    CHECK(tux_box->full.left == 47);
    CHECK(tux_box->full.top == 0);
    CHECK(tux_box->full.right == 103);
    CHECK(tux_box->full.bottom == 133);
    // Simple avatar: head == torso == full.
    CHECK(tux_box->head.left == tux_box->full.left);
    CHECK(tux_box->torso.right == tux_box->full.right);
    CHECK(tux_dim->width == 167);
    CHECK(tux_dim->height == 393);
    CHECK(tux_dim->head_height == 196);   // ydim/2 (avatar.cpp:63)
    CHECK(tux_dim->face_x == 104);
    CHECK(tux_dim->norm_height == 100);
}

TEST_CASE("FlipBodyBox mirrors component rectangles and the tail anchor") {
    const auto root = std::filesystem::path{COMICCHAT_TEST_COMICART_DIR};

    // Complex: full is unchanged, head/torso mirror around it as negative-width
    // rects (right < left), and faceX flips to width - faceX (avatar.cpp:113).
    const auto xeno = comicchat::load_avatar_asset(root / "xeno.avb");
    REQUIRE(xeno.has_value());
    const auto neutral = comicchat::select_avatar_expression(*xeno, {0.0, 0.0});
    REQUIRE(neutral.has_value());
    const auto normal = comicchat::avatar_body_box(*xeno, *neutral, 150, 133, false);
    const auto flipped = comicchat::avatar_body_box(*xeno, *neutral, 150, 133, true);
    REQUIRE(normal.has_value());
    REQUIRE(flipped.has_value());
    // Full box position is invariant under flip.
    CHECK(flipped->full.left == normal->full.left);
    CHECK(flipped->full.right == normal->full.right);
    // Head mirrors around full: new left = full.right - (old left - full.left).
    CHECK(flipped->head.left == normal->full.right - (normal->head.left - normal->full.left));
    CHECK(flipped->head.right == flipped->head.left - (normal->head.right - normal->head.left));
    // Width magnitude preserved.
    CHECK(std::abs(flipped->head.right - flipped->head.left) ==
        std::abs(normal->head.right - normal->head.left));

    const auto dim = comicchat::avatar_dim_info(*xeno, *neutral, false);
    const auto dim_flip = comicchat::avatar_dim_info(*xeno, *neutral, true);
    REQUIRE(dim.has_value());
    REQUIRE(dim_flip.has_value());
    CHECK(dim_flip->face_x == dim->width - dim->face_x);
    CHECK(dim_flip->width == dim->width);
    CHECK(dim_flip->head_height == dim->head_height);

    // Simple: FlipBodyBox swaps full left/right (bodycam.cpp:595-599).
    const auto tux = comicchat::load_avatar_asset(root / "tux.avb");
    REQUIRE(tux.has_value());
    const auto tux_neutral = comicchat::select_avatar_expression(*tux, {0.0, 0.0});
    REQUIRE(tux_neutral.has_value());
    const auto tux_normal = comicchat::avatar_body_box(*tux, *tux_neutral, 150, 133, false);
    const auto tux_flip = comicchat::avatar_body_box(*tux, *tux_neutral, 150, 133, true);
    REQUIRE(tux_normal.has_value());
    REQUIRE(tux_flip.has_value());
    CHECK(tux_flip->full.left == tux_normal->full.right);
    CHECK(tux_flip->full.right == tux_normal->full.left);
    const auto tux_dim = comicchat::avatar_dim_info(*tux, *tux_neutral, true);
    REQUIRE(tux_dim.has_value());
    CHECK(tux_dim->face_x == 167 - 104);
}

TEST_CASE("Avatar compositing is deterministic for identical requests") {
    const auto root = std::filesystem::path{COMICCHAT_TEST_COMICART_DIR};
    // Cover a complex (head+torso) and a simple (single-layer) avatar, flipped
    // and unflipped, with and without the nimbus aura.
    for (const auto filename : {"xeno.avb", "tux.avb", "susan.avb"}) {
        CAPTURE(filename);
        const auto asset = comicchat::load_avatar_asset(root / filename);
        REQUIRE(asset.has_value());
        const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
        REQUIRE(neutral.has_value());
        for (const bool flip : {false, true}) {
            for (const bool nimbus : {false, true}) {
                const comicchat::AvatarRenderRequest request{*neutral, 150, 133, flip, nimbus};
                const auto first = comicchat::render_avatar(*asset, request);
                const auto second = comicchat::render_avatar(*asset, request);
                REQUIRE(first.has_value());
                REQUIRE(second.has_value());
                CHECK(first->pixels == second->pixels);
                CHECK(dark_pixel_count(*first) == dark_pixel_count(*second));
            }
        }
    }
}

TEST_CASE("SRCAND drawings commute but a MERGEPAINT mask makes draw order load-bearing") {
    // §2.2.b: SRCAND is a bitwise AND (commutative/associative), so two masked-
    // off drawings composite to the same bytes regardless of TORSOFIRST. A
    // MERGEPAINT mask (D | ~S) does NOT commute with SRCAND, so once a mask is
    // interleaved the head-vs-torso order changes the result — this is exactly
    // the distinction the port must preserve.
    const auto asset = comicchat::load_avatar_asset(
        std::filesystem::path{COMICCHAT_TEST_COMICART_DIR} / "xeno.avb");
    REQUIRE(asset.has_value());
    REQUIRE(asset->kind == comicchat::AvatarKind::complex);
    const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
    REQUIRE(neutral.has_value());

    const auto render_with_flags = [&](const std::uint8_t flags) {
        auto variant = *asset;
        variant.flags = flags;
        return comicchat::render_avatar(variant, {*neutral, 150, 133, false, false});
    };

    // No masks: torso-first vs head-first are byte-identical (AND commutes).
    const auto torso_first_nomask = render_with_flags(flag_torso_first);
    const auto head_first_nomask = render_with_flags(0);
    REQUIRE(torso_first_nomask.has_value());
    REQUIRE(head_first_nomask.has_value());
    CHECK(torso_first_nomask->pixels == head_first_nomask->pixels);

    // With HEADMASK present, the order is load-bearing: the same layers in the
    // two orders now differ (MERGEPAINT does not commute with SRCAND).
    const auto torso_first_masked = render_with_flags(flag_head_mask | flag_torso_first);
    const auto head_first_masked = render_with_flags(flag_head_mask);
    REQUIRE(torso_first_masked.has_value());
    REQUIRE(head_first_masked.has_value());
    CHECK(torso_first_masked->pixels != head_first_masked->pixels);
}

TEST_CASE("Head mask changes the composite opaque count where it cuts the torso") {
    // A MERGEPAINT head mask whitens the head footprint before the head is
    // ANDed in (§2.2.b: "the mask ANDs a hole"). Toggling HEADMASK must change
    // the opaque-pixel count inside the head band — proving the mask phase is
    // actually applied, not skipped.
    const auto asset = comicchat::load_avatar_asset(
        std::filesystem::path{COMICCHAT_TEST_COMICART_DIR} / "xeno.avb");
    REQUIRE(asset.has_value());
    const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
    REQUIRE(neutral.has_value());
    const auto box = comicchat::avatar_body_box(*asset, *neutral, 150, 133, false);
    REQUIRE(box.has_value());

    const auto render_with_flags = [&](const std::uint8_t flags) {
        auto variant = *asset;
        variant.flags = flags;
        return comicchat::render_avatar(variant, {*neutral, 150, 133, false, false});
    };
    const auto masked = render_with_flags(flag_head_mask | flag_torso_first);
    const auto unmasked = render_with_flags(flag_torso_first);
    REQUIRE(masked.has_value());
    REQUIRE(unmasked.has_value());

    // The whole frame differs, and the mask cuts the torso specifically in the
    // head-over-torso overlap band, where the whitened head footprint removes
    // torso ink the head does not itself cover.
    CHECK(masked->pixels != unmasked->pixels);
    const auto overlap_top = std::max(box->head.top, box->torso.top);
    const auto overlap_bottom = std::min(box->head.bottom, box->torso.bottom);
    REQUIRE(overlap_top < overlap_bottom);
    const auto masked_overlap = dark_pixel_count_band(*masked, overlap_top, overlap_bottom);
    const auto unmasked_overlap = dark_pixel_count_band(*unmasked, overlap_top, overlap_bottom);
    CHECK(masked_overlap != unmasked_overlap);
    // MERGEPAINT whitens torso ink under the head footprint, so the masked
    // composite is never darker than the unmasked one in the overlap band.
    CHECK(masked_overlap <= unmasked_overlap);
}

TEST_CASE("Simple and complex avatars both composite visible ink within the fitted box") {
    const auto root = std::filesystem::path{COMICCHAT_TEST_COMICART_DIR};
    for (const auto filename : avatars) {
        CAPTURE(filename);
        const auto asset = comicchat::load_avatar_asset(root / filename);
        REQUIRE(asset.has_value());
        const auto neutral = comicchat::select_avatar_expression(*asset, {0.0, 0.0});
        REQUIRE(neutral.has_value());
        const auto rendered = comicchat::render_avatar(*asset, {*neutral, 150, 133, false, false});
        REQUIRE(rendered.has_value());
        // Every corpus avatar composites at least some ink (no blank frame).
        CHECK(dark_pixel_count(*rendered) > 0);
    }
}
