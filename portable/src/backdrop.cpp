#include "comicchat/backdrop.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace comicchat {
namespace {

// vector2d.h:46 ROUND -- round-half-away-from-zero. Reproduced locally
// (layout.cpp and page.cpp each carry their own copy of the same helper for
// the same reason: it is a tiny, header-free port of a one-line C macro, not
// worth a shared utility header).
[[nodiscard]] auto round_half(const double value) noexcept -> std::int32_t {
    return value > 0.0 ? static_cast<std::int32_t>(value + 0.5) : static_cast<std::int32_t>(value - 0.5);
}

// Case-insensitive ASCII compare key, mirroring SetBackDrop's stricmp lookup
// (backdrop.cpp:108) and GetCurrentBackDropID's stricmp scan (backdrop.cpp:
// 187). Deliberately byte-wise ASCII (not locale-aware): backdrop filenames
// are ASCII (the shipped comicart corpus), and a locale-aware fold would make
// catalog lookups depend on process locale state, which nothing else in this
// port does.
[[nodiscard]] auto ascii_lower(const std::string_view text) -> std::string {
    std::string result{text};
    std::ranges::transform(result, result.begin(),
        [](const char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
    return result;
}

} // namespace

BackdropCatalog::BackdropCatalog(std::filesystem::path dir) : dir_{std::move(dir)} {
    // records_[0] is the reserved "no backdrop" slot, mirroring
    // InitializeBackDrops seeding backRecS[0] with NULL (backdrop.cpp:166-173).
    records_.emplace_back();

    std::error_code error;
    if (!std::filesystem::is_directory(dir_, error) || error) return; // fail-soft, matches GetAllBackDropNames == 0
    std::filesystem::directory_iterator iterator{dir_, error};
    if (error) return;

    std::vector<std::string> filenames;
    for (const auto& entry : iterator) {
        std::error_code file_error;
        if (!entry.is_regular_file(file_error) || file_error) continue;
        if (entry.path().extension() != ".bgb") continue;
        filenames.push_back(entry.path().filename().string());
    }
    // Deterministic id assignment independent of host filesystem iteration
    // order (see the class doc comment for why sorted order is fine here).
    std::ranges::sort(filenames);

    records_.reserve(filenames.size() + 1);
    name_to_id_.reserve(filenames.size());
    for (auto& filename : filenames) {
        BackdropRecord record;
        record.filename = filename;
        record.back_id = static_cast<std::uint16_t>(records_.size()); // 1-based, mirrors backRecS.Add (backdrop.cpp:96)
        name_to_id_.emplace(ascii_lower(filename), record.back_id);
        records_.push_back(std::move(record));
    }
}

auto BackdropCatalog::id_for_name(const std::string_view name) const -> std::uint16_t {
    const auto it = name_to_id_.find(ascii_lower(name));
    return it == name_to_id_.end() ? 0 : it->second;
}

auto BackdropCatalog::name_for_id(const std::uint16_t back_id) const noexcept -> std::optional<std::string_view> {
    if (back_id == 0 || back_id >= records_.size()) return std::nullopt;
    return records_[back_id].filename;
}

auto BackdropCatalog::record_for_id(const std::uint16_t back_id) const noexcept -> const BackdropRecord* {
    if (back_id == 0 || back_id >= records_.size()) return nullptr;
    return &records_[back_id];
}

auto BackdropCatalog::resolve_art(const std::uint16_t back_id)
    -> std::expected<std::reference_wrapper<const AvatarBitmap>, BackdropArtError> {
    if (back_id == 0) return std::unexpected{BackdropArtError::no_such_id}; // "No backDrop for ID 0" (backdrop.cpp:270)

    if (const auto cached = art_cache_.find(back_id); cached != art_cache_.end())
        return std::cref(cached->second);

    const auto* record = record_for_id(back_id);
    if (record == nullptr) return std::unexpected{BackdropArtError::no_such_id};

    auto asset = load_avatar_asset(dir_ / record->filename);
    if (!asset) return std::unexpected{BackdropArtError::load_failed};
    if (asset->kind != AvatarKind::backdrop || !asset->backdrop.has_value())
        return std::unexpected{BackdropArtError::not_a_backdrop};

    const auto inserted = art_cache_.emplace(back_id, std::move(*asset->backdrop)).first;
    return std::cref(inserted->second);
}

void BackdropCatalog::flush_art(const std::uint16_t back_id) { art_cache_.erase(back_id); }

void BackdropCatalog::flush_all_art() { art_cache_.clear(); }

auto select_backdrop_art(BackdropCatalog& catalog, const std::uint16_t back_id)
    -> std::expected<std::reference_wrapper<const AvatarBitmap>, BackdropArtError> {
    return catalog.resolve_art(back_id);
}

auto crop_for_panel(const AvatarBitmap& art, const Rect& world_coords, const Rect& panel_bbox) noexcept
    -> BackdropCrop {
    const auto panel_width = static_cast<double>(panel_bbox.right - panel_bbox.left);
    const auto panel_height = static_cast<double>(panel_bbox.bottom - panel_bbox.top);
    const auto bit_width = static_cast<double>(art.width);
    const auto bit_height = static_cast<double>(art.height);

    std::int32_t src_left = 0;
    std::int32_t src_right = 0;
    if (panel_width != 0.0) {
        src_left = round_half(static_cast<double>(world_coords.left) / panel_width * bit_width);
        src_right = round_half(static_cast<double>(world_coords.right) / panel_width * bit_width);
    }

    std::int32_t src_top = 0;
    std::int32_t src_bottom = 0;
    if (panel_height != 0.0) {
        src_top = round_half(static_cast<double>(world_coords.top) / panel_height * bit_height);
        src_bottom = round_half(static_cast<double>(world_coords.bottom) / panel_height * bit_height);
    }

    return BackdropCrop{.src = Rect{.left = src_left, .bottom = src_bottom, .right = src_right, .top = src_top}};
}

} // namespace comicchat
