#include "comicchat/page.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace comicchat {
namespace {

// vector2d.h:27 LARGEINTEGER, panel.cpp GetInterveningBBox clamps.
inline constexpr int large_integer = 100000000;

// Dock() delta (balloon.cpp:587): TOPBORDER + YBORDER + HWAVEHEIGHT.
inline constexpr int dock_delta = balloon_topborder + balloon_yborder + balloon_hwave_height;

// GetBalloonRect (panel.cpp:842): the drawable balloon region, the top half of the
// panel interior, inset by the border pen.
[[nodiscard]] auto balloon_free_rect(const PageConfig& cfg) -> Rect {
    Rect r{0, -cfg.unit_height / 2, cfg.unit_width, 0};
    r.left += cfg.border_width;    // brect.left  += penWidth
    r.right -= cfg.border_width;   // brect.right -= penWidth
    r.top -= cfg.border_width;     // brect.top   -= penWidth
    return r;
}

// CLabel::WidestWord (balloon.cpp:736): the width of the widest whitespace-
// delimited run, measured with the same TextMeasure the wrap uses.
[[nodiscard]] auto widest_word(const TextMeasure& measure, std::string_view text) -> int {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    int widest = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_space(text[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && !is_space(text[i])) {
            ++i;
        }
        if (i > start) {
            widest = std::max(widest, measure(text.substr(start, i - start)));
        }
    }
    return widest;
}

// The mutable per-balloon state GetInterveningBBox / AdjustRouteRgns read and
// write: the true cloud bbox (immutable, from GetCloudBBox) plus the shrinking
// route region (m_routeRgn) and the speaker anchor.
struct PlacedBalloon final {
    int arrow_x{};
    Rect cloud_bbox{};   // GetCloudBBox, immutable
    int route_left{};    // m_routeRgn.Left, shrinks via SetRouteRgn
    int route_right{};   // m_routeRgn.Right, shrinks via SetRouteRgn
    int bbox_bottom{};   // m_bbox.Bottom, for LowestPreviousBottom
};

// CBalloon::QueryRouteRgn (balloon.cpp:1430): the horizontal clearance this
// already-placed balloon leaves for a new balloon whose tail sits at other_to_x.
void query_route_rgn(const PlacedBalloon& b, int other_to_x, int& left_allow, int& right_allow) {
    const int to_x = b.arrow_x;
    if (other_to_x > to_x) {
        left_allow = std::max(to_x, b.route_left + balloon_min_route_width);
        right_allow = large_integer;
    } else {
        left_allow = -large_integer;
        right_allow = std::min(to_x, b.route_right - balloon_min_route_width);
    }
}

// CBalloon::SetRouteRgn (balloon.cpp:1447): subtract a newer balloon's span from
// this balloon's route region so tails stop overlapping.
void set_route_rgn(PlacedBalloon& b, int other_to_x, int left, int right) {
    const int to_x = b.arrow_x;
    if (other_to_x > to_x) {
        b.route_right = std::min(b.route_right, left);
    } else {
        b.route_left = std::max(b.route_left, right);
    }
}

// GetInterveningBBox (panel.cpp:167): shift/clamp irect horizontally out of the
// prior balloons' route regions, then lower irect.top beneath the prior clouds.
void get_intervening_bbox(const std::vector<PlacedBalloon>& placed, const Rect& free_rect,
                          Rect& irect, int this_arrow_x) {
    const int to_pt_x = this_arrow_x;
    int most_left = free_rect.left;
    int most_right = free_rect.right;
    for (const auto& b : placed) {
        int left_allow = 0;
        int right_allow = 0;
        query_route_rgn(b, to_pt_x, left_allow, right_allow);
        most_left = std::max(left_allow, most_left);
        most_right = std::min(right_allow, most_right);
    }
    if (most_left > irect.left || most_right < irect.right) {
        const int potential_clearance = most_right - most_left;
        if (potential_clearance >= (irect.right - irect.left)) {
            const int delta = most_left > irect.left ? most_left - irect.left : most_right - irect.right;
            irect.left += delta;
            irect.right += delta;
        } else {
            irect.left = most_left;
            irect.right = most_right;
        }
    }

    irect.top = free_rect.top;
    for (const auto& b : placed) {
        Rect cloudbox = b.cloud_bbox;
        if (cloudbox.right < irect.left) {  // cloud is to the right (left in Y-up? source x-compare)
            irect.top = std::min(irect.top, cloudbox.top);
        } else {
            cloudbox.top += dock_delta;     // Dock (balloon.cpp:585)
            cloudbox.bottom += dock_delta;
            irect.top = std::min(irect.top, cloudbox.bottom);
        }
    }
}

// AdjustRouteRgns (panel.cpp:248): subtract the just-placed balloon's route span
// from every earlier balloon's route region.
void adjust_route_rgns(std::vector<PlacedBalloon>& placed, int this_arrow_x, int left, int right) {
    for (auto& b : placed) {
        set_route_rgn(b, this_arrow_x, left, right);
    }
}

// LowestPreviousBottom (panel.cpp:214): lowest m_bbox.Bottom among placed
// balloons, starting from low_y (freeRect.top).
[[nodiscard]] auto lowest_previous_bottom(const std::vector<PlacedBalloon>& placed, int low_y) -> int {
    for (const auto& b : placed) {
        low_y = std::min(low_y, b.bbox_bottom);
    }
    return low_y;
}

[[nodiscard]] auto modes_to_kind(std::uint16_t modes) -> BalloonShapeKind {
    return select_balloon_mode(modes);
}

// ForceFitBalloon (panel.cpp:153) + CLabel::SplitHeight (balloon.cpp:794): when a
// lone balloon is too tall for the free rect, pin it to the full rect and split
// the overflowing lines into a leftover string the caller re-adds.
//
// HONEST LIMIT: SplitHeight splits at the exact line the balloon overflows; this
// approximates that at portable line granularity (keep the lines that fit the
// free-rect height, rejoin the rest with spaces). Exact byte-for-byte split
// points depend on the source's goalWidth re-wrap the portable model does not do.
[[nodiscard]] auto force_fit_split(const BalloonRequest& request, const Rect& free_rect,
                                   std::string& leftover) -> Balloon {
    const int height = free_rect.top - free_rect.bottom;
    const int line_height = std::max(1, request.font.line_height);
    int max_lines = (height - request.font.base_add) / line_height;
    max_lines = std::max(1, max_lines);

    BalloonRequest fit = request;
    fit.place_left = free_rect.left;
    fit.place_top = free_rect.top;
    if (static_cast<int>(request.lines.size()) > max_lines) {
        fit.lines.assign(request.lines.begin(), request.lines.begin() + max_lines);
        std::string rest;
        for (std::size_t i = static_cast<std::size_t>(max_lines); i < request.lines.size(); ++i) {
            if (!rest.empty()) {
                rest.push_back(' ');
            }
            rest += request.lines[i].text;
        }
        leftover = std::move(rest);
    } else {
        leftover.clear();
    }
    return layout_balloon(fit);
}

// Distinct speaker ids in first-appearance (element) order. Within a panel each
// avatar speaks at most once (AvatarInPanel forces a new panel), so this is the
// balloon/element order LayoutBalloons iterates.
[[nodiscard]] auto speak_order_of(const std::vector<Line>& elements) -> std::vector<std::uint32_t> {
    std::vector<std::uint32_t> order;
    for (const auto& line : elements) {
        if (std::find(order.begin(), order.end(), line.speaker.avatar_id) == order.end()) {
            order.push_back(line.speaker.avatar_id);
        }
    }
    return order;
}

} // namespace

Page::Page(PageConfig config, TextMeasure measure)
    : config_{std::move(config)}, measure_{std::move(measure)}, seed_rng_{config_.seed} {}

auto Page::panels() const noexcept -> const std::vector<Panel>& { return panels_; }
auto Page::panel_infos() const noexcept -> const std::vector<PanelInfo>& { return infos_; }

void Page::register_avatar(const PageAvatar& selection) {
    registry_[selection.avatar_id] = selection;
}

void Page::add_participant(const PageAvatar& selection) { register_avatar(selection); }

// CUnitPanelPage::AddLine (panel.cpp:1061).
void Page::add_line(const Line& line) {
    register_avatar(line.speaker);

    const bool is_action = line.modes == bm_action;  // panel.cpp:1067 (BM_ACTION only)
    if (is_action) {
        new_panel_pending_ = true;  // StartNewPanel()
    }

    const PanelState* old = states_.empty() ? nullptr : &states_.back();
    const bool speaker_in_panel =
        old != nullptr &&
        std::any_of(old->info.body_order.begin(), old->info.body_order.end(),
                    [&](std::uint32_t id) { return id == line.speaker.avatar_id; });

    const PanelSplitState split_state{old != nullptr ? old->elements.size() : 0U, states_.size(),
                                      new_panel_pending_};
    const bool start_new = should_start_new_panel(split_state, speaker_in_panel, is_action);

    PanelState draft;
    bool replace_last = false;
    if (start_new) {
        draft.seed = seed_rng_.next();  // m_seed = rand() (panel.cpp:558)
        draft.elements = {line};
        draft.historesis_in = states_.empty() ? HistoresisMap{} : states_.back().historesis_out;
        new_panel_pending_ = false;
    } else {
        draft = *old;  // CPanel::Clone (panel.cpp:564): copies elements + seed + historesis_in
        draft.elements.push_back(line);
        replace_last = true;
    }

    const auto outcome = layout_panel(draft);
    if (outcome.status == Fit::overflow) {
        // panel.cpp:1116-1118: delete the clone, StartNewPanel, retry this line.
        new_panel_pending_ = true;
        add_line(line);
        return;
    }

    if (replace_last) {
        states_.back() = std::move(draft);
        panels_.back() = states_.back().rendered;
        infos_.back() = states_.back().info;
    } else {
        states_.push_back(std::move(draft));
        panels_.push_back(states_.back().rendered);
        infos_.push_back(states_.back().info);
    }

    if (outcome.status == Fit::fit_with_leftover) {
        // panel.cpp:1131-1138: recurse on the split-off remainder.
        Line rest = line;
        rest.text = outcome.leftover;
        add_line(rest);
    }
}

// CUnitPanel::LayoutAvatars (panel.cpp:728) + LayoutBalloons (panel.cpp:858).
auto Page::layout_panel(PanelState& draft) -> Page::LayoutOutcome {
    // ---- LayoutAvatars: order + place the bodies. -----------------------------
    const auto speakers = speak_order_of(draft.elements);

    std::vector<ConversationAvatar> avatars;
    avatars.reserve(registry_.size());
    for (const auto& [id, sel] : registry_) {
        avatars.push_back(ConversationAvatar{id, sel.talk_tos});
    }

    const auto order = order_conversation(speakers, avatars, draft.historesis_in);
    draft.historesis_out = order.historesis;

    // Body slots in placement order (panel.cpp:759-775 collect widths).
    std::vector<BodySlot> slots;
    slots.reserve(order.bodies.size());
    int body_width_sum = 0;
    for (const auto& placed : order.bodies) {
        const auto& sel = registry_.at(placed.avatar_id);
        slots.push_back(BodySlot{placed.avatar_id, sel.body_width, sel.face_fraction, placed.flip});
        body_width_sum += sel.body_width;
    }

    // margin = (m_unitWidth - bdyWidth) / (bdyCount+1) (panel.cpp:810).
    const int body_count = static_cast<int>(slots.size());
    const int margin = (config_.unit_width - body_width_sum) / (body_count + 1);
    const auto anchors = arrow_anchors(slots, 0, margin);

    // Place bodies + build the avatar -> (arrow_x, box, flip, color) maps.
    struct BodyPlacement final {
        int arrow_x{};
        Rect box{};
        bool flip{};
        std::uint32_t color{};
    };
    std::map<std::uint32_t, BodyPlacement> placements;
    draft.rendered = Panel{};
    draft.rendered.seed = draft.seed;
    draft.info = PanelInfo{};
    draft.info.seed = draft.seed;
    draft.info.speak_order = speakers;

    for (std::size_t i = 0; i < order.bodies.size(); ++i) {
        const auto& placed = order.bodies[i];
        const auto& anchor = anchors[i];
        const auto& sel = registry_.at(placed.avatar_id);
        // SetBBox (panel.cpp:816): Bottom = panel floor, Top = floor + height.
        Rect box{anchor.left, -config_.unit_height, anchor.left + sel.body_width,
                 -config_.unit_height + sel.body_height};
        placements[placed.avatar_id] = BodyPlacement{anchor.arrow_x, box, placed.flip, sel.color};
        draft.info.body_order.push_back(placed.avatar_id);

        PanelBody body{};
        body.avatar_id = placed.avatar_id;
        body.box = box;
        body.arrow_x = anchor.arrow_x;
        body.color = sel.color;
        body.flip = placed.flip;
        draft.rendered.bodies.push_back(body);
    }

    // ---- LayoutBalloons: seed one PRNG per panel, lay each balloon. -----------
    MsvcrtRandom rng{draft.seed};  // srand(m_seed) (panel.cpp:870)
    const Rect free_rect = balloon_free_rect(config_);
    std::vector<PlacedBalloon> placed_balloons;

    const int nb = static_cast<int>(draft.elements.size());
    for (int i = 0; i < nb; ++i) {
        const auto& line = draft.elements[static_cast<std::size_t>(i)];
        const auto& place = placements.at(line.speaker.avatar_id);
        const auto kind = modes_to_kind(line.modes);
        const bool is_box = kind.mode == BalloonMode::action;

        const auto lines = break_into_lines(measure_, config_.max_text_width, line.text);
        const int text_extent = measure_(line.text);

        CloudEstimateInput in{};
        in.text_extent = text_extent;
        in.text_height = config_.font.line_height;  // single-line cy ~= line_height
        in.line_height = config_.font.line_height;
        in.widest_word = widest_word(measure_, line.text);
        in.free_left = free_rect.left;
        in.free_right = free_rect.right;
        in.free_top = free_rect.top;
        in.free_bottom = free_rect.bottom;
        in.lowest_prev_bottom = lowest_previous_bottom(placed_balloons, free_rect.top);
        in.arrow_x = place.arrow_x;
        in.is_box = is_box;

        // *** PRNG order, step 1: GetCloudEstimate (panel.cpp:888). Draws the
        //     goalWidth (multi-line only) then the x-overlap start (non-box). ***
        const auto est = cloud_estimate(rng, in);

        Rect brect{est.left, 0, est.right, free_rect.top};
        get_intervening_bbox(placed_balloons, free_rect, brect, place.arrow_x);

        BalloonRequest req{};
        req.kind = kind;
        req.text = line.text;
        req.lines = lines;
        req.font = config_.font;
        req.arrow_x = place.arrow_x;
        req.speaker_top = place.box.top;
        req.place_left = brect.left;
        req.place_top = brect.top;

        Balloon balloon = layout_balloon(req);
        // Draw the text at the same font pixel size the cloud was fitted to.
        balloon.text_size = config_.text_size;

        // *** PRNG order, step 2: SetBBox -> ShiftLines (balloon.cpp:768) draws
        //     one randfloat() per laid-out line. shift_line_offsets drops these
        //     draws (the offset is deterministic, jitter == 0), so the page driver
        //     advances the PRNG here to keep the exact multi-balloon rand() order.
        //     THIS CLOSES THE 2.1 MEDIUM (shift_line_offsets rng desync). ***
        for (std::size_t k = 0; k < balloon.lines.size(); ++k) {
            (void)rng.next();
        }

        // LayoutBalloon fit gate (panel.cpp:944): the cloud must clear the hook.
        const bool fits = balloon.route_region.bottom >= free_rect.bottom + min_hook_height;
        if (!fits) {
            if (i == 0 && nb == 1) {
                // ForceFitBalloon (panel.cpp:153) + SplitHeight remainder.
                std::string leftover;
                Balloon forced = force_fit_split(req, free_rect, leftover);
                forced.text_size = config_.text_size;
                draft.rendered.balloons.push_back(std::move(forced));
                return {Fit::fit_with_leftover, leftover};
            }
            return {Fit::overflow, {}};
        }

        // Success: record the cloud/route state for the following balloons.
        PlacedBalloon record{};
        record.arrow_x = place.arrow_x;
        record.cloud_bbox = balloon.route_region;  // GetCloudBBox
        record.route_left = balloon.route_region.left;
        record.route_right = balloon.route_region.right;
        record.bbox_bottom = balloon.bbox.bottom;
        // AdjustRouteRgns (panel.cpp:946): subtract this span from earlier ones.
        adjust_route_rgns(placed_balloons, place.arrow_x, record.route_left, record.route_right);
        placed_balloons.push_back(record);

        draft.rendered.balloons.push_back(std::move(balloon));
    }

    return {Fit::fit, {}};
}

} // namespace comicchat
