#include "comicchat/layout.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>

namespace comicchat {
namespace {

// One body inside the greedy placement working set. `talk_tos` is copied from
// the avatar registry so EvalPair can consult it for both spoken and pulled-in
// bodies; `flip` is the facing being trialled/chosen.
struct WorkBody final {
    std::uint32_t avatar_id{};
    std::vector<std::uint32_t> talk_tos{};
    bool requested{true};
    bool flip{};
};

// GetAvatar(id)->m_lastDir/... — a missing avatar takes the source defaults
// (avatar.cpp:896-898: facing right, neighbour ids 0).
[[nodiscard]] auto historesis_for(const HistoresisMap& memory, const std::uint32_t avatar_id)
    -> AvatarHistoresis {
    if (const auto found = memory.find(avatar_id); found != memory.end()) {
        return found->second;
    }
    return AvatarHistoresis{};
}

[[nodiscard]] auto find_avatar(const std::vector<ConversationAvatar>& avatars,
                               const std::uint32_t avatar_id) -> const ConversationAvatar* {
    for (const auto& avatar : avatars) {
        if (avatar.avatar_id == avatar_id) {
            return &avatar;
        }
    }
    return nullptr;
}

// ComputeDisplacementPenalty (panel.cpp:260): one point each time a body's left
// or right neighbour differs from the neighbour it remembered last panel — the
// force that keeps a stable left/right arrangement across panels.
[[nodiscard]] auto compute_displacement_penalty(const std::vector<WorkBody>& order,
                                                 const HistoresisMap& memory) -> int {
    int penalty = 0;
    const auto count = static_cast<int>(order.size());
    for (int i = 0; i < count; ++i) {
        const auto memoryFor = historesis_for(memory, order[i].avatar_id);
        if (i > 0 && memoryFor.last_right != order[i - 1].avatar_id) {
            ++penalty;
        }
        if (i < count - 1 && memoryFor.last_left != order[i + 1].avatar_id) {
            ++penalty;
        }
    }
    return penalty;
}

// EvalPair (panel.cpp:280): rate b1's facing relative to b2, `delta` positions
// apart (signed: positive = b2 to the right of b1). Rewards facing the people you
// address and adjacency; heavily penalises facing away from someone you address.
[[nodiscard]] auto eval_pair(const WorkBody& b1, const WorkBody& b2, int delta) -> int {
    int rating = 0;
    bool desired_dir = false;  // FALSE = face right
    if (delta > 0) {
        desired_dir = false;
    } else {
        desired_dir = true;
        delta = -delta;
    }

    if (b1.talk_tos.empty()) {
        // Talking "to the world".
        if (b1.flip != desired_dir) {
            rating += 4;  // I'm not facing the other's direction
        }
        if (b2.flip == desired_dir) {
            rating += 2;  // he's not facing my direction
        }
    } else {
        for (const auto target : b1.talk_tos) {
            if (target == b2.avatar_id) {
                if (b1.flip == desired_dir) {
                    rating += 4 * (delta - 1);  // facing him: reward proportional to adjacency
                } else {
                    rating += 40;  // facing away from someone I address: heavy penalty
                }
                if (b2.flip == desired_dir) {
                    rating += 4;  // he faces away while I address him: minor penalty
                }
            }
        }
    }
    return rating;
}

// EvalPlacement (panel.cpp:359): score inserting `body` at `index`, trying both
// facings, and report the cheaper one via `dir`. Ties resolve to the avatar's
// remembered facing (m_lastDir) — the only "tie-break", and it is deterministic,
// so no PRNG is consulted here.
[[nodiscard]] auto eval_placement(std::vector<WorkBody> order, const int placed_count,
                                  const WorkBody& body, const int index,
                                  const HistoresisMap& memory, bool& dir) -> int {
    order.insert(order.begin() + index, body);
    const int penalty = compute_displacement_penalty(order, memory);
    int rating_right = penalty;
    int rating_left = penalty;

    const auto sum_pairs = [&order, placed_count]() {
        int rating = 0;
        for (int i = 0; i <= placed_count; ++i) {
            for (int j = i + 1; j <= placed_count; ++j) {
                rating += eval_pair(order[static_cast<std::size_t>(i)],
                                    order[static_cast<std::size_t>(j)], j - i);
                rating += eval_pair(order[static_cast<std::size_t>(j)],
                                    order[static_cast<std::size_t>(i)], i - j);
            }
        }
        return rating;
    };

    order[static_cast<std::size_t>(index)].flip = false;  // facing right
    rating_right += sum_pairs();

    order[static_cast<std::size_t>(index)].flip = true;  // facing left
    rating_left += sum_pairs();

    if (rating_right < rating_left) {
        dir = false;
        return rating_right;
    }
    if (rating_right > rating_left) {
        dir = true;
        return rating_left;
    }
    dir = historesis_for(memory, body.avatar_id).last_dir;
    return rating_right;
}

// DoGreedyOrdering (panel.cpp:405): insert each body at the position + facing
// with the minimum rating, keeping the first best position (strict `<`).
[[nodiscard]] auto do_greedy_ordering(const std::vector<WorkBody>& bodies,
                                      const HistoresisMap& memory) -> std::vector<WorkBody> {
    std::vector<WorkBody> order;
    int placed_count = 0;
    for (const auto& body : bodies) {
        int best_rating = 1000;
        int best_position = 0;
        bool best_dir = false;
        for (int position = 0; position <= placed_count; ++position) {
            bool dir = false;
            const int rating = eval_placement(order, placed_count, body, position, memory, dir);
            if (rating < best_rating) {
                best_rating = rating;
                best_position = position;
                best_dir = dir;
            }
        }
        WorkBody chosen = body;
        chosen.flip = best_dir;
        order.insert(order.begin() + best_position, chosen);
        ++placed_count;
    }
    return order;
}

// AddTalkTos (panel.cpp:317): with room to spare, pull each speaker's addressed
// partners into the panel (skipping duplicates and unknown avatars), capped at
// five bodies total. Only the initial speakers are scanned for partners.
void add_talk_tos(std::vector<WorkBody>& bodies, const std::vector<ConversationAvatar>& avatars) {
    const auto initial_count = bodies.size();
    for (std::size_t i = 0; i < initial_count; ++i) {
        const auto partners = bodies[i].talk_tos;  // copy: bodies may reallocate below
        for (const auto target : partners) {
            if (bodies.size() >= 5) {
                return;  // never more than five people in a panel
            }
            const bool duplicate = std::any_of(bodies.begin(), bodies.end(),
                                               [target](const WorkBody& existing) {
                                                   return existing.avatar_id == target;
                                               });
            if (duplicate) {
                continue;
            }
            const auto* partner = find_avatar(avatars, target);
            if (partner == nullptr) {
                continue;  // no such avatar (source ASSERTs and skips)
            }
            bodies.push_back(WorkBody{partner->avatar_id, partner->talk_tos, false, false});
        }
    }
}

// UpdateHistoresis (panel.cpp:437): remember each placed body's facing and its
// left/right neighbours for the next panel. End bodies keep their prior neighbour
// on the open side; a lone body updates only its facing.
[[nodiscard]] auto update_historesis(const std::vector<WorkBody>& order, const HistoresisMap& prior)
    -> HistoresisMap {
    HistoresisMap next = prior;
    const auto count = static_cast<int>(order.size());
    for (int i = 0; i < count; ++i) {
        auto& memoryFor = next[order[i].avatar_id];
        memoryFor.last_dir = order[i].flip;
        if (i > 0) {
            memoryFor.last_right = order[i - 1].avatar_id;
        }
        if (i < count - 1) {
            memoryFor.last_left = order[i + 1].avatar_id;
        }
    }
    return next;
}

// vector2d.h:46 ROUND — round-half-away-from-zero.
[[nodiscard]] auto round_half(const double value) -> int {
    return value > 0.0 ? static_cast<int>(value + 0.5) : static_cast<int>(value - 0.5);
}

} // namespace

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

auto fit_panel_transform(const std::int32_t canvas_width, const std::int32_t canvas_height,
                         const double source_units) -> PanelTransform {
    if (canvas_width <= 0 || canvas_height <= 0) {
        throw std::invalid_argument{"canvas dimensions must be positive"};
    }
    if (!(source_units > 0.0)) {
        throw std::invalid_argument{"source_units must be positive"};
    }
    const auto panel_size = static_cast<double>(std::min(canvas_width, canvas_height));
    const auto scale = panel_size / source_units;
    const auto origin_x = (static_cast<double>(canvas_width) - panel_size) / 2.0;
    const auto origin_y = (static_cast<double>(canvas_height) - panel_size) / 2.0;
    return {origin_x, origin_y, scale};
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

auto order_conversation(const std::vector<std::uint32_t>& speaker_ids,
                        const std::vector<ConversationAvatar>& avatars,
                        const HistoresisMap& historesis) -> ConversationOrder {
    // Seed the working set with the speaking bodies, in panel/element order.
    std::vector<WorkBody> bodies;
    bodies.reserve(speaker_ids.size());
    for (const auto avatar_id : speaker_ids) {
        const auto* speaker = find_avatar(avatars, avatar_id);
        auto talk_tos = speaker != nullptr ? speaker->talk_tos : std::vector<std::uint32_t>{};
        bodies.push_back(WorkBody{avatar_id, std::move(talk_tos), true, false});
    }

    // OrderAvatars (panel.cpp:426): pull in talk-to partners when there is room.
    if (bodies.size() < 5) {
        add_talk_tos(bodies, avatars);
    }

    const auto placed = do_greedy_ordering(bodies, historesis);

    ConversationOrder result;
    result.bodies.reserve(placed.size());
    for (const auto& body : placed) {
        result.bodies.push_back(PlacedBody{body.avatar_id, body.requested, body.flip});
    }
    result.historesis = update_historesis(placed, historesis);
    return result;
}

auto should_start_new_panel(const PanelSplitState& state, const bool speaker_already_in_panel,
                            const bool is_action_box) -> bool {
    if (is_action_box) {
        return true;  // BM_ACTION forces StartNewPanel (panel.cpp:1067)
    }
    return state.new_panel_pending || state.tail_panel_elements >= 5 || state.panel_count < 2 ||
           speaker_already_in_panel;
}

auto arrow_anchors(const std::vector<BodySlot>& slots, const std::int32_t interior_left,
                   const std::int32_t gap) -> std::vector<ArrowAnchor> {
    std::vector<ArrowAnchor> anchors;
    anchors.reserve(slots.size());
    std::int32_t cursor = interior_left + gap;
    for (const auto& slot : slots) {
        const double fraction = slot.flip ? 1.0 - slot.face_fraction : slot.face_fraction;
        const auto arrow = cursor + round_half(fraction * static_cast<double>(slot.width));
        anchors.push_back(ArrowAnchor{slot.avatar_id, cursor, arrow});
        cursor += slot.width + gap;
    }
    return anchors;
}

} // namespace comicchat
