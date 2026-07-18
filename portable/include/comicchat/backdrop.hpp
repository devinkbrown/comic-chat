#pragma once

// Backdrop scene-background CATALOG + panel-crop math, ported from Microsoft's
// v2.5-beta-1-modern/backdrop.cpp and backdrop.h.
//
// Backdrop *parsing* (the .bgb -> AvatarBitmap decode) already exists:
// parse_backdrop() (avatar_assets.cpp:717), dispatched from AssetParser::parse()
// when AvatarKind::backdrop (avatar_assets.cpp:413), decodes into
// AvatarAsset::backdrop (avatar_assets.hpp:68). This header supplies the parts
// Microsoft built around that decode:
//   * the name<->id catalog (BDFileRec, backdrop.cpp:28-40; GetAllBackDropNames'
//     directory scan, backdrop.cpp:47-80; SetBackDropAux id assignment and
//     GetBackDropNameFromID lookup, backdrop.cpp:85-98,397-407),
//   * the id->art loader/cache (GetBackDropArtFromID / backMapS,
//     backdrop.cpp:267-283), and
//   * the world-coordinate panel-crop ratio math (CBackDrop::Draw's
//     srcLeft/srcTop/srcRight/srcBottom, backdrop.cpp:330-368, especially
//     the world_coords -> source-bitmap mapping at backdrop.cpp:347-350).
//
// Deliberately OUT of scope for this pass (sequenced separately):
//   * URL-based backdrop download (backdrop.cpp:141-156, network-bound),
//   * the print-path screen/printer cache split (backMapS/backMapP,
//     backdrop.cpp:232-283,297-309; the portable renderer has no print path),
//   * the actual Cairo blit and the render_panel draw-hook / Panel.backdrop_id
//     wiring (backdrop.cpp:330-368's StretchBlt equivalent) -- this file only
//     produces the *inputs* (a decoded AvatarBitmap and a computed source
//     crop rect) that hook will eventually consume.

#include "comicchat/avatar_assets.hpp"
#include "comicchat/layout.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace comicchat {

enum class BackdropArtError { no_such_id, load_failed, not_a_backdrop };

// The standard backdrop-art world extent every BDFileRec is initialized to
// (SetBackDropAux, backdrop.cpp:91-95: worldLeft = worldTop = 0, worldRight =
// 4860, worldBottom = -4860). `Rect` (layout.hpp) uses the same Left/Bottom/
// Right/Top, Y-up world-coordinate convention as MS's SRECT (bbox.h:1-9).
inline constexpr Rect default_backdrop_world_coords{.left = 0, .bottom = -4860, .right = 4860, .top = 0};

// Mirrors BDFileRec (backdrop.cpp:28-40) minus the URL/download fields
// (pszURL is set only after a network fetch completes, backdrop.cpp:90,
// 373-395 -- out of scope here) and minus xdim/ydim/normHeight, which the
// source never reads back out of the record (they exist only to be written).
// `filename` is the whole on-disk filename including extension, matching
// GetAllBackDropNames' `strFiles.Add(fd.name)` (backdrop.cpp:66, "We are now
// adding the whole filename"). `mode` mirrors CBackDrop::m_mode / BF_NOZOOM
// (backdrop.h:36,43); it is not consulted by crop_for_panel itself (see that
// function's docs) -- it is state the render_panel draw-hook (sequenced
// separately) will read to decide what `world_coords` to pass in.
struct BackdropRecord final {
    std::string filename;
    std::uint16_t back_id{};
    Rect world_coords{default_backdrop_world_coords};
    std::uint8_t mode{};
};

// Scans a directory for `.bgb` backdrop files and assigns/looks up stable
// ids, replacing GetAllBackDropNames' Win32 _findfirst/_findnext directory
// scan (backdrop.cpp:47-80) with std::filesystem::directory_iterator -- the
// same portable pattern comic_page.cpp's has_avatar() already uses for `.avb`.
// Only `.bgb` is scanned: BACKDROPTYPE_SEARCHMASK's legacy `*.bmp` backdrop
// type (backdrop.cpp:20-23) has no members in the shipped comicart corpus
// (v2.5-beta-1-modern/comicart/*.bgb) and is out of scope.
//
// Id 0 is reserved ("no backdrop", mirroring backRecS[0] == NULL after
// InitializeBackDrops, backdrop.cpp:166-173, and GetBackDropArtFromID's
// `if (!backID) return NULL;`, backdrop.cpp:270). Ids are then assigned
// 1-based in *sorted filename order*. The source's own scan order followed
// whatever _findfirst/_findnext happened to return, which the underlying
// filesystem never actually guaranteed either; sorting makes catalog ids
// reproducible across hosts and filesystems, which matters here because nothing
// in this port needs byte-identical ids with a running MFC client (there is no
// shared wire protocol for backdrop ids), only self-consistent ids across runs
// on the same corpus.
class BackdropCatalog final {
public:
    // Builds the catalog by scanning `dir`. Fails soft to an empty catalog
    // (only the reserved id-0 slot) on a missing or unreadable directory,
    // matching GetAllBackDropNames returning 0 when _findfirst finds nothing
    // (backdrop.cpp:61-79) rather than raising.
    explicit BackdropCatalog(std::filesystem::path dir);

    [[nodiscard]] auto directory() const noexcept -> const std::filesystem::path& { return dir_; }

    // All discovered records, index 0 unused/default (the reserved id).
    [[nodiscard]] auto records() const noexcept -> const std::vector<BackdropRecord>& { return records_; }

    // 0 = "no backdrop" / not found, mirroring GetBackDropArtFromID's id-0
    // guard (backdrop.cpp:270) reused here for name lookup misses too.
    [[nodiscard]] auto id_for_name(std::string_view name) const -> std::uint16_t;

    // Mirrors GetBackDropNameFromID's backRecS[nID] lookup (backdrop.cpp:
    // 397-407); nullopt for id 0 or an id past the end of the catalog (MS's
    // raw array index would instead read backRecS[nID] out of bounds).
    [[nodiscard]] auto name_for_id(std::uint16_t back_id) const noexcept -> std::optional<std::string_view>;

    [[nodiscard]] auto record_for_id(std::uint16_t back_id) const noexcept -> const BackdropRecord*;

    // Resolves `back_id` to its decoded art, loading + caching on a miss.
    // Mirrors GetBackDropArtFromID + BackDropArtFromBackID (backdrop.cpp:
    // 242-283): looks up the in-memory cache first (backMapS's
    // CMapWordToPtr::Lookup), and on a miss loads the .bgb via the existing
    // load_avatar_asset/parse_backdrop path and caches the result (map->SetAt).
    // There is deliberately no screen/printer cache split here (backMapS vs
    // backMapP) -- the portable renderer has no print path.
    [[nodiscard]] auto resolve_art(std::uint16_t back_id)
        -> std::expected<std::reference_wrapper<const AvatarBitmap>, BackdropArtError>;

    // Mirrors FlushBackDropFromID (backdrop.cpp:286-295).
    void flush_art(std::uint16_t back_id);

    // Mirrors FlushBackDropCache (backdrop.cpp:297-309).
    void flush_all_art();

private:
    std::filesystem::path dir_;
    std::vector<BackdropRecord> records_;                    // records_[0] is the reserved id-0 slot
    std::unordered_map<std::string, std::uint16_t> name_to_id_; // lower-cased filename -> id
    std::unordered_map<std::uint16_t, AvatarBitmap> art_cache_;
};

// Thin free-function wrapper over BackdropCatalog::resolve_art, matching
// GetBackDropArtFromID's call shape (backdrop.cpp:267: `GetBackDropArtFromID
// (backID, toScreen)`) for callers that prefer a function over a method.
[[nodiscard]] auto select_backdrop_art(BackdropCatalog& catalog, std::uint16_t back_id)
    -> std::expected<std::reference_wrapper<const AvatarBitmap>, BackdropArtError>;

// The result of cropping a backdrop bitmap to the world-coordinate window a
// panel wants to show. Unlike every other use of `Rect` in this codebase
// (world-space, Y-up), `src` here holds the *bitmap's own pixel-space*
// offsets (origin top-left, rows increasing downward) -- exactly the
// srcLeft/srcTop/srcRight/srcBottom Win32 StretchBlt source rect
// CBackDrop::Draw computes (backdrop.cpp:347-352) before blitting. Reusing
// the plain four-field `Rect` POD for both purposes mirrors the source's own
// reuse of generic SRECT/RECT for both world boxes and pixel rects; the
// meaning is carried by the call site and doc comment, not the type.
struct BackdropCrop final {
    Rect src;
};

// Pure port of CBackDrop::Draw's srcLeft/srcTop/srcRight/srcBottom ratio math
// (backdrop.cpp:347-350):
//
//   srcLeft   = ROUND(bbox.Left   / panelWidth  * bitWidth)
//   srcTop    = ROUND(bbox.Top    / panelHeight * bitHeight)
//   srcRight  = ROUND(bbox.Right  / panelWidth  * bitWidth)
//   srcBottom = ROUND(bbox.Bottom / panelHeight * bitHeight)
//   (panelWidth = panelRect->right - panelRect->left;
//    panelHeight = panelRect->bottom - panelRect->top;)
//
// `world_coords` is the crop window a panel instance wants shown -- what MS
// calls the CBackDrop panel element's own `m_bbox` (set per panel by
// AdjustArtToCoord, panel.cpp:951-958) -- and `panel_bbox` is the *full*
// panel's own world-coordinate box (MS's `panelRect`, e.g. CUnitPanel::Draw's
// `{0, 0, unitWidth, -unitHeight}`, panel.cpp:670-672). Both share the same
// Left/Bottom/Right/Top, Y-up unit system as `world_coords`; the ratio math
// is unit-agnostic (X and Y are each normalized against the panel's own
// extent before being rescaled into the bitmap's pixel grid), which is why a
// world-space Y-up numerator divided by a world-space Y-up denominator still
// lands correctly in the bitmap's Y-down pixel space -- the sign flip in Top/
// Bottom cancels the matching sign flip in panelHeight.
//
// ROUND is round-half-away-from-zero (vector2d.h:46), reproduced here (and
// already reproduced independently in layout.cpp/page.cpp for their own
// ports of the same macro).
//
// BF_NOZOOM (backdrop.h:36) never appears inside this ratio math in the
// source: CBackDrop::Draw does not branch on m_mode at all. Instead,
// AdjustArtToCoord forces zoomFactor to 1.0 under BF_NOZOOM *before*
// computing m_bbox (panel.cpp:951-958), so a BF_NOZOOM crop window is simply
// the degenerate case world_coords == panel_bbox: the ratios collapse to
// [0, bitWidth] x [0, bitHeight], i.e. the entire source bitmap with no pan
// or zoom. Callers that manage `mode` (the render_panel draw-hook, sequenced
// separately) select that world_coords themselves; this function stays a
// pure function of its three inputs, deliberately mirroring the source's own
// lack of a mode branch at this layer.
//
// Fails soft on a degenerate `panel_bbox` (zero width and/or zero height --
// which would otherwise divide by zero): the corresponding axis' source
// offsets are left at 0 rather than computed from an infinite/NaN ratio.
[[nodiscard]] auto crop_for_panel(const AvatarBitmap& art, const Rect& world_coords, const Rect& panel_bbox) noexcept
    -> BackdropCrop;

} // namespace comicchat
