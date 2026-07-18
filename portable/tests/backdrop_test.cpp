#include "comicchat/backdrop.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string_view>

#ifndef COMICCHAT_TEST_COMICART_DIR
#error "COMICCHAT_TEST_COMICART_DIR must point at Microsoft's active Comic Chat corpus"
#endif

namespace {

using comicchat::Rect;

// The full-panel world box every default BDFileRec/CBackDrop is initialized
// to (SetBackDropAux, backdrop.cpp:91-95; CBackDrop's constructor,
// backdrop.h:41). Used below both as `panel_bbox` (MS's panelRect,
// panel.cpp:670-672) and, in the BF_NOZOOM case, as `world_coords` too.
constexpr Rect standard_panel{.left = 0, .bottom = -4860, .right = 4860, .top = 0};

} // namespace

TEST_CASE("crop_for_panel reproduces CBackDrop::Draw's identity crop (BF_NOZOOM)") {
    // AdjustArtToCoord forces zoomFactor to 1.0 under BF_NOZOOM (panel.cpp:952),
    // which collapses m_bbox to exactly the full panel box (panel.cpp:954-958
    // with delta == 0). world_coords == panel_bbox is therefore the faithful
    // stand-in for "BF_NOZOOM": no pan, no zoom, the whole source bitmap.
    const comicchat::AvatarBitmap art{.width = 315, .height = 200, .pixels = {}};
    const auto crop = comicchat::crop_for_panel(art, standard_panel, standard_panel);
    CHECK(crop.src == Rect{.left = 0, .bottom = 200, .right = 315, .top = 0});
}

TEST_CASE("crop_for_panel reproduces CBackDrop::Draw's centered zoom crop") {
    // A world_coords window centered on the standard panel, covering exactly
    // its middle 50% on both axes (zoom factor 2): left = 4860/4, right =
    // 4860*3/4, top = -4860/4, bottom = -4860*3/4.
    constexpr Rect zoomed_window{.left = 1215, .bottom = -3645, .right = 3645, .top = -1215};
    const comicchat::AvatarBitmap art{.width = 315, .height = 200, .pixels = {}};
    const auto crop = comicchat::crop_for_panel(art, zoomed_window, standard_panel);
    // srcLeft   = ROUND(1215 / 4860 * 315) = ROUND(78.75)  = 79
    // srcRight  = ROUND(3645 / 4860 * 315) = ROUND(236.25) = 236
    // srcTop    = ROUND(-1215 / -4860 * 200) = ROUND(50.0)  = 50
    // srcBottom = ROUND(-3645 / -4860 * 200) = ROUND(150.0) = 150
    CHECK(crop.src == Rect{.left = 79, .bottom = 150, .right = 236, .top = 50});
}

TEST_CASE("crop_for_panel reproduces CBackDrop::Draw's asymmetric pan") {
    // A non-centered crop window: only the left/top quarter is panned in,
    // right/bottom stay at the panel edge (zoom on one axis only, x panned).
    constexpr Rect panned_window{.left = 972, .bottom = -4860, .right = 4860, .top = 0};
    const comicchat::AvatarBitmap art{.width = 400, .height = 250, .pixels = {}};
    const auto crop = comicchat::crop_for_panel(art, panned_window, standard_panel);
    // srcLeft   = ROUND(972 / 4860 * 400)  = ROUND(80.0)  = 80
    // srcRight  = ROUND(4860 / 4860 * 400) = ROUND(400.0) = 400
    // srcTop    = ROUND(0 / -4860 * 250)   = ROUND(0.0)   = 0
    // srcBottom = ROUND(-4860 / -4860 * 250) = ROUND(250.0) = 250
    CHECK(crop.src == Rect{.left = 80, .bottom = 250, .right = 400, .top = 0});
}

TEST_CASE("crop_for_panel fails soft on a degenerate (zero-extent) panel_bbox") {
    // A zero-width, zero-height panel_bbox would divide the ratio math by
    // zero (backdrop.cpp:347-350's panelWidth/panelHeight denominators);
    // crop_for_panel must not produce inf/NaN -> UB on the (int) cast.
    constexpr Rect degenerate_panel{.left = 100, .bottom = -50, .right = 100, .top = -50};
    const comicchat::AvatarBitmap art{.width = 315, .height = 200, .pixels = {}};
    const auto crop = comicchat::crop_for_panel(art, standard_panel, degenerate_panel);
    CHECK(crop.src == Rect{.left = 0, .bottom = 0, .right = 0, .top = 0});
}

TEST_CASE("crop_for_panel fails soft when only one axis of panel_bbox is degenerate") {
    // Zero width but a normal height: the X axis must fall back to 0 while
    // the Y axis still computes the identity crop from `standard_panel`.
    constexpr Rect zero_width_panel{.left = 100, .bottom = -4860, .right = 100, .top = 0};
    const comicchat::AvatarBitmap art{.width = 315, .height = 200, .pixels = {}};
    const auto crop = comicchat::crop_for_panel(art, standard_panel, zero_width_panel);
    CHECK(crop.src == Rect{.left = 0, .bottom = 200, .right = 0, .top = 0});
}

TEST_CASE("BackdropCatalog assigns stable ids over Microsoft's active BGB corpus") {
    constexpr std::array<std::string_view, 7> expected_backdrops{
        "buckroom.bgb", "clouds.bgb", "field.bgb", "pastoral.bgb", "room.bgb", "space.bgb", "yellow.bgb",
    };

    comicchat::BackdropCatalog catalog{std::filesystem::path{COMICCHAT_TEST_COMICART_DIR}};

    // records() holds the reserved id-0 slot plus one entry per discovered
    // .bgb file; the corpus is not exclusively backdrops (it also ships
    // .avb avatars), so only the expected 7 should be found.
    REQUIRE(catalog.records().size() == expected_backdrops.size() + 1);

    for (const auto name : expected_backdrops) {
        CAPTURE(name);
        const auto id = catalog.id_for_name(name);
        CHECK(id != 0);
        REQUIRE(catalog.name_for_id(id).has_value());
        CHECK(*catalog.name_for_id(id) == name);
        CHECK(catalog.record_for_id(id) != nullptr);
        CHECK(catalog.record_for_id(id)->filename == name);
        CHECK(catalog.record_for_id(id)->world_coords == comicchat::default_backdrop_world_coords);
    }

    // Case-insensitive lookup, mirroring SetBackDrop's stricmp (backdrop.cpp:108).
    CHECK(catalog.id_for_name("ROOM.BGB") == catalog.id_for_name("room.bgb"));

    // Id 0 is reserved and never assigned to a real file.
    CHECK(catalog.id_for_name("no-such-backdrop.bgb") == 0);
    CHECK_FALSE(catalog.name_for_id(0).has_value());
    CHECK(catalog.record_for_id(0) == nullptr);

    // Ids stay stable and correspond to alphabetically sorted filenames
    // (deterministic regardless of host directory-iteration order).
    std::array<std::uint16_t, expected_backdrops.size()> ids{};
    for (std::size_t index = 0; index < expected_backdrops.size(); ++index) {
        ids[index] = catalog.id_for_name(expected_backdrops[index]);
    }
    CHECK(std::ranges::is_sorted(ids));
}

TEST_CASE("BackdropCatalog::resolve_art loads and caches real BGB art") {
    comicchat::BackdropCatalog catalog{std::filesystem::path{COMICCHAT_TEST_COMICART_DIR}};
    const auto id = catalog.id_for_name("room.bgb");
    REQUIRE(id != 0);

    const auto first = catalog.resolve_art(id);
    REQUIRE(first.has_value());
    CHECK(first->get().width > 0);
    CHECK(first->get().height > 0);
    const auto* const first_ptr = &first->get();
    const auto first_width = first->get().width;
    const auto first_height = first->get().height;

    // Second resolution must hit the cache and return the same bitmap.
    const auto second = catalog.resolve_art(id);
    REQUIRE(second.has_value());
    CHECK(&second->get() == first_ptr);

    // select_backdrop_art is a thin forwarding wrapper.
    const auto via_free_function = comicchat::select_backdrop_art(catalog, id);
    REQUIRE(via_free_function.has_value());
    CHECK(&via_free_function->get() == first_ptr);

    // Flushing invalidates `first_ptr` (the cache entry is erased); resolve
    // again afterward for a freshly re-decoded bitmap and only compare
    // content, not identity -- the allocator is free to reuse the same
    // address for the new node (as it evidently does on some standard
    // library implementations), so pointer (in)equality is not a meaningful
    // "was it re-decoded" signal.
    catalog.flush_art(id);
    const auto after_flush = catalog.resolve_art(id);
    REQUIRE(after_flush.has_value());
    CHECK(after_flush->get().width == first_width);
    CHECK(after_flush->get().height == first_height);

    catalog.flush_all_art();
}

TEST_CASE("BackdropCatalog::resolve_art rejects id 0 and unknown ids") {
    comicchat::BackdropCatalog catalog{std::filesystem::path{COMICCHAT_TEST_COMICART_DIR}};
    const auto reserved = catalog.resolve_art(0);
    REQUIRE_FALSE(reserved.has_value());
    CHECK(reserved.error() == comicchat::BackdropArtError::no_such_id);

    const auto unknown = catalog.resolve_art(9999);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error() == comicchat::BackdropArtError::no_such_id);
}

TEST_CASE("BackdropCatalog fails soft on a missing directory") {
    comicchat::BackdropCatalog catalog{std::filesystem::path{"/nonexistent/comic-chat-backdrop-dir"}};
    CHECK(catalog.records().size() == 1); // only the reserved id-0 slot
    CHECK(catalog.id_for_name("room.bgb") == 0);
}
