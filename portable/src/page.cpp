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

// vector2d.h:46 ROUND — round-half-away-from-zero. Duplicated locally from
// layout.cpp's (internal-linkage) helper of the same name to avoid a
// cross-translation-unit dependency for one three-line function.
[[nodiscard]] auto round_half(const double value) -> int {
    return value > 0.0 ? static_cast<int>(value + 0.5) : static_cast<int>(value - 0.5);
}

// HONEST LIMIT (page.hpp): the portable avatar pipeline carries no measured
// per-avatar head-pixel metric, so an unset PageAvatar::head_height falls back
// to this fraction of body_height. 0.32 approximates a standing figure's
// head:body ratio closely enough to keep the "don't cut at neck" zoom cap
// (panel.cpp:797) meaningfully conservative rather than a no-op.
inline constexpr double head_height_body_fraction = 0.32;

// GetDimInfo normHeight fallback (panel.cpp:761): unset (0) means this
// avatar's single composited pose normalizes to its own full body height.
[[nodiscard]] auto effective_norm_height(const PageAvatar& sel) -> std::int32_t {
    return sel.norm_height > 0 ? sel.norm_height : sel.body_height;
}

// GetDimInfo headHeight fallback (panel.cpp:761); see head_height_body_fraction.
[[nodiscard]] auto effective_head_height(const PageAvatar& sel) -> std::int32_t {
    if (sel.head_height > 0) {
        return sel.head_height;
    }
    return round_half(static_cast<double>(sel.body_height) * head_height_body_fraction);
}

// One placed body's final scaled geometry (LayoutAvatars panel.cpp:759-806).
// `top` is the box top-anchor Y (the avatar's head), = MS top[i] (panel.cpp:772,
// 787): computed from the normalized/shrunk height and, in the zoom branch,
// NOT recomputed — so a zoomed body grows DOWNWARD (feet sink below the floor,
// clipped by the panel) while the head stays pinned just under the balloon,
// exactly as SetBBox (panel.cpp:816) does. Pinning the feet instead floats the
// head up into the balloon region (the over-zoom regression).
struct ScaledBody final {
    std::int32_t width{};
    std::int32_t height{};
    std::int32_t top{};
};

// CUnitPanel::LayoutAvatars body scaling (panel.cpp:740,759-819): normalize
// every placed body onto a common maxBodyHeight, then either shrink (combined
// width overflows the panel) or zoom in (config.zoom_avatars, and the source's
// Establishing() is always false for a chat panel) to fill unused panel width,
// capped so a zoomed head is never cropped.
//
// Head-anchoring (panel.cpp:772,787,816): top[i] is set to -unitHeight+height
// after normalize (and recomputed after shrink), but is NOT recomputed after the
// zoom branch. SetBBox then anchors the box TOP (head) at top[i] and puts the
// bottom (feet) at top[i]-height. So a zoomed body keeps its head pinned just
// under the balloon and grows downward, its feet sinking below the panel floor
// where the panel clip crops them. This is faithful and — unlike pinning the
// feet to the floor — keeps the head on-panel instead of shoving it up behind
// the balloon (the "balloon covers the avatar" over-zoom).
[[nodiscard]] auto scale_avatar_bodies(const std::vector<PlacedBody>& order_bodies,
                                       const std::map<std::uint32_t, PageAvatar>& registry,
                                       const PageConfig& config) -> std::vector<ScaledBody> {
    const std::size_t count = order_bodies.size();
    std::vector<ScaledBody> scaled(count);
    if (count == 0) {
        return scaled;
    }

    // maxBodyHeight = (int)(m_unitHeight / 1.9) (panel.cpp:740): a C-style
    // truncating cast (toward zero), not a round.
    const auto max_body_height =
        static_cast<std::int32_t>(static_cast<double>(config.unit_height) / 1.9);

    std::vector<std::int32_t> raw_width(count);
    std::vector<std::int32_t> raw_height(count);
    std::vector<std::int32_t> norm_height(count);
    std::vector<std::int32_t> head_height(count);
    std::int32_t max_norm = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& sel = registry.at(order_bodies[i].avatar_id);
        raw_width[i] = sel.body_width;
        raw_height[i] = sel.body_height;
        norm_height[i] = effective_norm_height(sel);
        head_height[i] = effective_head_height(sel);
        max_norm = std::max(max_norm, norm_height[i]);
    }
    // maxNorm <= 0 only if every placed body has a zero/negative body_height
    // (real avatar art never is); guard the divide the source has no need to.
    if (max_norm <= 0) {
        for (std::size_t i = 0; i < count; ++i) {
            scaled[i] = ScaledBody{raw_width[i], raw_height[i],
                                   -config.unit_height + raw_height[i]};
        }
        return scaled;
    }

    std::vector<std::int32_t> width(count);
    std::vector<std::int32_t> height(count);
    // top[i] head-anchor (panel.cpp:772): -unitHeight + height, set after
    // normalize, recomputed after shrink, left stale through zoom.
    std::vector<std::int32_t> top(count);
    std::int32_t sum_width = 0;
    for (std::size_t i = 0; i < count; ++i) {
        // panel.cpp:768-774: scale every body so its normHeight maps onto the
        // shared maxBodyHeight, carrying width and headHeight along by the same
        // ratio.
        const auto new_height = round_half(static_cast<double>(max_body_height) *
                                           (static_cast<double>(norm_height[i]) /
                                            static_cast<double>(max_norm)));
        const double scale_ratio = raw_height[i] != 0
                                       ? static_cast<double>(new_height) / static_cast<double>(raw_height[i])
                                       : 0.0;
        height[i] = new_height;
        width[i] = round_half(scale_ratio * static_cast<double>(raw_width[i]));
        head_height[i] = round_half(scale_ratio * static_cast<double>(head_height[i]));
        top[i] = -config.unit_height + height[i];  // panel.cpp:772
        sum_width += width[i];
    }

    // sumWidth = bdyWidth + (bdyCount+1)*minMargin (panel.cpp:777); minMargin is
    // always 0 in the portable driver, so sumWidth == bdyWidth already.
    if (sum_width > 0 && sum_width > config.unit_width) {
        // panel.cpp:780-789: shrink every body to fit the panel width.
        const double reduction = static_cast<double>(config.unit_width) / static_cast<double>(sum_width);
        for (std::size_t i = 0; i < count; ++i) {
            height[i] = round_half(static_cast<double>(height[i]) * reduction);
            width[i] = round_half(static_cast<double>(width[i]) * reduction);
            top[i] = -config.unit_height + height[i];  // panel.cpp:787 (recomputed)
        }
    } else if (sum_width > 0 && config.zoom_avatars) {
        // panel.cpp:791-806: zoom every body up to fill unused panel width,
        // capped so the tallest head isn't cropped.
        double zoom_factor = static_cast<double>(config.unit_width) / static_cast<double>(sum_width);
        std::int32_t max_head_height = 0;
        for (std::size_t i = 0; i < count; ++i) {
            max_head_height = std::max(max_head_height, head_height[i]);
        }
        if (max_head_height > 0) {
            const double head_factor =
                static_cast<double>(max_body_height) / (static_cast<double>(max_head_height) * 1.2);
            zoom_factor = std::min(zoom_factor, head_factor);
        }
        if (zoom_factor < 1.1) {
            zoom_factor = 1.0;  // panel.cpp:799: not worth a tiny zoom.
        }
        for (std::size_t i = 0; i < count; ++i) {
            height[i] = round_half(static_cast<double>(height[i]) * zoom_factor);
            width[i] = round_half(static_cast<double>(width[i]) * zoom_factor);
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        scaled[i] = ScaledBody{width[i], height[i], top[i]};
    }
    return scaled;
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

    // Body slots in placement order, fed the SCALED widths (panel.cpp:759-819:
    // collect raw dims, normalize onto maxBodyHeight, then shrink or zoom).
    const auto scaled_bodies = scale_avatar_bodies(order.bodies, registry_, config_);
    std::vector<BodySlot> slots;
    slots.reserve(order.bodies.size());
    int body_width_sum = 0;
    for (std::size_t i = 0; i < order.bodies.size(); ++i) {
        const auto& placed = order.bodies[i];
        const auto& sel = registry_.at(placed.avatar_id);
        slots.push_back(BodySlot{placed.avatar_id, scaled_bodies[i].width, sel.face_fraction, placed.flip});
        body_width_sum += scaled_bodies[i].width;
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
        const auto& dims = scaled_bodies[i];
        // SetBBox (panel.cpp:816): Bottom = panel floor, Top = floor + height.
        // (Bottom is always the floor here, not the source's literal stale
        // top[i]-height[i] after a zoom — see scale_avatar_bodies' doc comment.)
        // SetBBox (panel.cpp:816): head pinned at dims.top, feet at top-height
        // (below the floor when zoomed; the panel clip crops them).
        Rect box{anchor.left, dims.top - dims.height, anchor.left + dims.width,
                 dims.top};
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

        // Formatting seam (page.hpp Line::runs): with no runs this is the exact
        // plain break_into_lines call as before (zero behavior change); with runs
        // the formatted overload attaches each line's slice on top of the SAME
        // wrap result, so line text/width/count and the PRNG walk below are
        // unaffected. The caller supplies runs already parsed against line.text.
        const auto lines = line.runs.empty()
                               ? break_into_lines(measure_, config_.max_text_width, line.text)
                               : break_into_lines_formatted(measure_, config_.max_text_width, line.text,
                                                            line.runs);
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
                // CLabel::SplitHeight returns NULL when the forced balloon did
                // not actually split (balloon.cpp:1556), and AddLine recurses
                // only for a non-NULL remainder (panel.cpp:1128). Preserve that
                // distinction: an empty portable remainder completes this panel
                // instead of manufacturing a second, empty balloon panel.
                const auto status = leftover.empty() ? Fit::fit : Fit::fit_with_leftover;
                return {status, std::move(leftover)};
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
