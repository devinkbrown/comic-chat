#pragma once

#include "comicchat/cpp26.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace comicchat {

inline constexpr std::int32_t logical_panel_width = 2300;
inline constexpr std::int32_t logical_panel_height = 2300;
inline constexpr std::int32_t logical_interstice = 144;
inline constexpr std::size_t panels_per_row = 2;

struct Rect final {
    std::int32_t left{};
    std::int32_t bottom{};
    std::int32_t right{};
    std::int32_t top{};
    auto operator==(const Rect&) const -> bool = default;
};

struct Participant final {
    std::string name;
    bool is_self{};
    bool departed{};
    std::uint32_t sends{};
    bool has_icon{true};
};

[[nodiscard]] auto panel_rect(std::size_t panel_index, std::int32_t left, std::int32_t top) -> Rect;
[[nodiscard]] auto page_bounds(std::size_t panel_count, std::int32_t left, std::int32_t top) -> Rect;
[[nodiscard]] auto order_stars(const std::vector<Participant>& participants, std::size_t max_stars)
    -> std::vector<std::size_t>;

} // namespace comicchat
