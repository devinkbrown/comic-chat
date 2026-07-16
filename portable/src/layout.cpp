#include "comicchat/layout.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace comicchat {

auto panel_rect(const std::size_t panel_index, const std::int32_t left, const std::int32_t top) -> Rect {
    const auto column = static_cast<std::int32_t>(panel_index % panels_per_row);
    const auto row = static_cast<std::int32_t>(panel_index / panels_per_row);
    const auto panel_left = left + column * (logical_panel_width + logical_interstice);
    const auto panel_top = top - row * (logical_panel_height + logical_interstice);
    return {panel_left, panel_top - logical_panel_height, panel_left + logical_panel_width, panel_top};
}

auto page_bounds(const std::size_t panel_count, const std::int32_t left, const std::int32_t top) -> Rect {
    if (panel_count == 0) {
        throw std::invalid_argument{"a comic page contains at least the title panel"};
    }
    const auto rows = static_cast<std::int32_t>((panel_count - 1) / panels_per_row + 1);
    const auto columns = static_cast<std::int32_t>(std::min(panel_count, panels_per_row));
    return {
        left,
        top - (rows * logical_panel_height + (rows - 1) * logical_interstice),
        left + columns * logical_panel_width + (columns - 1) * logical_interstice,
        top,
    };
}

auto order_stars(const std::vector<Participant>& participants, const std::size_t max_stars)
    -> std::vector<std::size_t> {
    std::vector<std::size_t> ordered;
    for (std::size_t candidate_index = 0; candidate_index < participants.size(); ++candidate_index) {
        const auto& candidate = participants[candidate_index];
        if (!candidate.has_icon) {
            continue;
        }
        if (candidate.is_self) {
            ordered.insert(ordered.begin(), candidate_index);
            continue;
        }
        auto insertion = ordered.end();
        for (auto current = std::next(ordered.begin(), std::min<std::size_t>(1, ordered.size()));
             current != ordered.end(); ++current) {
            const auto& existing = participants[*current];
            if ((!candidate.departed && existing.departed) ||
                (candidate.departed == existing.departed && candidate.sends > existing.sends)) {
                insertion = current;
                break;
            }
        }
        if (insertion != ordered.end()) {
            ordered.insert(insertion, candidate_index);
        } else if (max_stars > 0 && ordered.size() <= max_stars) {
            ordered.push_back(candidate_index);
        }
    }
    if (ordered.size() > max_stars) {
        ordered.resize(max_stars);
    }
    return ordered;
}

} // namespace comicchat
