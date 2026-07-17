#include "comicchat/balloon.hpp"

#include <algorithm>
#include <array>
#include <cmath>

// Item 2.1 balloon geometry — pure logic, no Cairo. render_panel (render.cpp)
// consumes this to emit into the logical drawing pass. Every routine is a
// line-for-line port of the cited panel.cpp / balloon.cpp / spline.cpp source so
// control-point streams reproduce byte-for-byte (render_test.cpp goldens them).

namespace comicchat {
namespace {

// vector2d.h:46 ROUND — round-half-away-from-zero (matches CvertsToCubic etc.).
[[nodiscard]] auto iround(const double value) noexcept -> int {
    return value > 0.0 ? static_cast<int>(value + 0.5) : static_cast<int>(value - 0.5);
}

struct DPoint final {
    double x{};
    double y{};
};

// vector2d.cpp POINT/DPOINT helpers with their exact rounding/truncation model.
[[nodiscard]] auto sub(const BalloonPoint a, const BalloonPoint b) noexcept -> BalloonPoint {
    return {a.x - b.x, a.y - b.y};
}
[[nodiscard]] auto add(const BalloonPoint a, const BalloonPoint b) noexcept -> BalloonPoint {
    return {a.x + b.x, a.y + b.y};
}
[[nodiscard]] auto to_d(const BalloonPoint p) noexcept -> DPoint { return {static_cast<double>(p.x), static_cast<double>(p.y)}; }
// point_scalmult(double, DPOINT) — keeps doubles (vector2d.cpp:25).
[[nodiscard]] auto scal(const double s, const DPoint p) noexcept -> DPoint { return {p.x * s, p.y * s}; }
// dpoint_to_point — ROUND both coordinates (vector2d.cpp:159).
[[nodiscard]] auto to_i(const DPoint p) noexcept -> BalloonPoint { return {iround(p.x), iround(p.y)}; }
[[nodiscard]] auto dist(const BalloonPoint a, const BalloonPoint b) noexcept -> double {
    const double dx = static_cast<double>(a.x) - static_cast<double>(b.x);
    const double dy = static_cast<double>(a.y) - static_cast<double>(b.y);
    return std::sqrt(dx * dx + dy * dy);
}
[[nodiscard]] auto vector_to_angle(const BalloonPoint v) noexcept -> double {
    // vector2d.cpp:143 — atan2(y, x), 0 for a degenerate vector.
    if (std::abs(v.x) < 1 && std::abs(v.y) < 1) return 0.0;
    return std::atan2(static_cast<double>(v.y), static_cast<double>(v.x));
}
[[nodiscard]] auto norm(const DPoint p) noexcept -> DPoint {
    const double magn = std::sqrt(p.x * p.x + p.y * p.y);
    if (magn < 1e-24) return {0.0, 0.0};
    return scal(1.0 / magn, p);
}

constexpr double pi = 3.14159265358979323846;

} // namespace

auto select_balloon_mode(const std::uint16_t modes) noexcept -> BalloonShapeKind {
    if ((modes & bm_action) != 0) {
        return {BalloonMode::action, (modes & bm_whisper) != 0};
    }
    if ((modes & bm_whisper) != 0) return {BalloonMode::whisper, true};
    if ((modes & bm_think) != 0) return {BalloonMode::think, false};
    return {BalloonMode::say, false};
}

auto shift_line_offsets(const std::vector<TextLine>& lines, const LabelJustify justify)
    -> std::vector<int> {
    // ShiftLines (balloon.cpp:768) with MAXLEFTSHIFT/MAXCENTERSHIFT == 0: the
    // random jitter term collapses to 0, leaving the deterministic offset.
    const int max_width = widest_line_width(lines);
    std::vector<int> offsets;
    offsets.reserve(lines.size());
    for (const auto& line : lines) {
        if (justify == LabelJustify::left) {
            offsets.push_back(0);
        } else {
            offsets.push_back((max_width - line.width) / 2);
        }
    }
    return offsets;
}

auto get_filters(const std::vector<TextLine>& lines, const std::vector<int>& left_x)
    -> FilterStaircases {
    FilterStaircases f;
    const int n = static_cast<int>(std::min(lines.size(), left_x.size()));
    if (n <= 0) return f;

    // l[nL] / r[nR] accumulators, indices nL/nR (balloon.cpp:484-527).
    f.left.push_back(FilterRun{0, 0, left_x[0], 0});
    f.right.push_back(FilterRun{0, 0, left_x[0] + lines[0].width, 0});

    const auto line_left = [&](const int i) { return left_x[i]; };
    const auto line_right = [&](const int i) { return left_x[i] + lines[static_cast<std::size_t>(i)].width; };

    for (int i = 1; i < n; ++i) {
        const int this_left = line_left(i);
        const int this_right = line_right(i);
        const int left_delta = this_left - f.left.back().x;
        const int right_delta = this_right - f.right.back().x;

        if (left_delta <= balloon_thresh1) {  // extends dramatically to the left
            f.left.back().end = i - 1;
            f.left.push_back(FilterRun{i, 0, this_left, 0});
        } else if (left_delta <= 0) {  // extends marginally to the left
            f.left.back().x = this_left;
        } else if (left_delta >= balloon_thresh2) {  // indents dramatically to the right
            const int next_left = (i + 1 < n) ? line_left(i + 1) : this_left;
            if (next_left - f.left.back().x >= balloon_thresh2) {
                f.left.back().end = i - 1;
                f.left.push_back(FilterRun{i, 0, std::min(this_left, next_left), 0});
            }
        }

        if (right_delta >= -balloon_thresh1) {  // extends dramatically to the right
            f.right.back().end = i - 1;
            f.right.push_back(FilterRun{i, 0, this_right, 0});
        } else if (right_delta >= 0) {  // extends marginally to the right
            f.right.back().x = this_right;
        } else if (right_delta <= -balloon_thresh2) {  // indents dramatically to the left
            const int next_right = (i + 1 < n) ? line_right(i + 1) : this_right;
            if (next_right - f.right.back().x <= -balloon_thresh2) {
                f.right.back().end = i - 1;
                f.right.push_back(FilterRun{i, 0, std::max(this_right, next_right), 0});
            }
        }
    }

    f.left.back().end = n - 1;
    f.right.back().end = n - 1;
    return f;
}

auto permute_filters(FilterStaircases& filters, const FontMetrics& font) -> int {
    const int line_height = font.line_height;
    const int base_add = font.base_add;
    const int top_offset = font.top_offset;
    constexpr int large_integer = 100000000;  // LARGEINTEGER (vector2d.h:27)

    int base_y = 0;
    int last_x = large_integer;
    for (std::size_t i = 0; i < filters.left.size(); ++i) {
        auto& run = filters.left[i];
        run.x -= balloon_xborder;
        if (i == 0) {
            run.y = base_y + balloon_topborder + balloon_yborder + top_offset;
        } else if (run.x < last_x) {
            run.y = base_y + balloon_yborder;
        } else {
            run.y = base_y - balloon_yborder - base_add;
        }
        base_y -= (run.end - run.start + 1) * line_height;
        last_x = run.x;
    }

    base_y = 0;
    last_x = -large_integer;
    for (std::size_t i = 0; i < filters.right.size(); ++i) {
        auto& run = filters.right[i];
        run.x += balloon_xborder;
        if (i == 0) {
            run.y = base_y + balloon_topborder + balloon_yborder + top_offset;
        } else if (run.x > last_x) {
            run.y = base_y + balloon_yborder;
        } else {
            run.y = base_y - balloon_yborder - base_add;
        }
        base_y -= (run.end - run.start + 1) * line_height;
        last_x = run.x;
    }
    return base_y - balloon_topborder - balloon_yborder - base_add;
}

void add_wavies(const BalloonPoint& pt1, const BalloonPoint& pt2, std::vector<BalloonPoint>& pts,
                const int wave_diam, const int interval) {
    const double d = dist(pt1, pt2);
    const double n_waves = d / interval;
    if (n_waves < 2) return;
    const int i_waves = static_cast<int>(n_waves);
    const double wave_len = d / i_waves;
    const DPoint unit = scal(1.0 / d, DPoint{to_d(pt2).x - to_d(pt1).x, to_d(pt2).y - to_d(pt1).y});
    const BalloonPoint inc = to_i(scal(wave_len, unit));
    const DPoint normal{unit.y, -unit.x};
    const BalloonPoint extra = to_i(scal(static_cast<double>(wave_diam), normal));
    BalloonPoint this_base = pt1;
    for (int i = 0; i < i_waves - 1; ++i) {
        this_base = add(this_base, inc);
        if ((i & 0x1) == 0) {
            pts.push_back(add(this_base, extra));
        } else {
            pts.push_back(this_base);
        }
    }
}

auto create_balloon_spline(const FilterStaircases& filters, const int final_y)
    -> std::vector<BalloonPoint> {
    std::vector<BalloonPoint> pts;
    const auto& lf = filters.left;
    const auto& rf = filters.right;
    int last_y = final_y;

    for (std::size_t i = 0; i < lf.size(); ++i) {
        BalloonPoint this_pt{lf[i].x, lf[i].y};
        if (i > 0) add_wavies(pts.back(), this_pt, pts, balloon_hwave_height, balloon_hwave_interval);
        pts.push_back(this_pt);
        BalloonPoint next_pt{lf[i].x, (i == lf.size() - 1) ? final_y : lf[i + 1].y};
        add_wavies(pts.back(), next_pt, pts, balloon_vwave_height, balloon_vwave_interval);
        pts.push_back(next_pt);
    }

    for (int i = static_cast<int>(rf.size()) - 1; i >= 0; --i) {
        BalloonPoint this_pt{rf[static_cast<std::size_t>(i)].x, last_y};
        add_wavies(pts.back(), this_pt, pts, balloon_hwave_height, balloon_hwave_interval);
        pts.push_back(this_pt);
        last_y = rf[static_cast<std::size_t>(i)].y;
        BalloonPoint next_pt{this_pt.x, last_y};
        add_wavies(pts.back(), next_pt, pts, balloon_vwave_height, balloon_vwave_interval);
        pts.push_back(next_pt);
    }

    if (!pts.empty()) add_wavies(pts.back(), pts.front(), pts, balloon_hwave_height, balloon_hwave_interval);
    return pts;
}

namespace {

using Matrix = std::array<std::array<double, 4>, 4>;

// CBeta::SetMatrix (spline.cpp:112) for (tension, bias).
[[nodiscard]] auto beta_matrix(const double tension, const double bias) -> Matrix {
    Matrix m{};
    const double b2 = bias * bias;
    const double b3 = bias * b2;
    const double d = 1.0 / (tension + (2.0 * b3) + (4.0 * (b2 + bias)) + 2.0);
    m[0][0] = -2.0 * b3;
    m[0][1] = 2.0 * (tension + b3 + b2 + bias);
    m[0][2] = -2.0 * (tension + b2 + bias + 1.0);
    m[1][0] = 6.0 * b3;
    m[1][1] = -3.0 * (tension + (2.0 * (b3 + b2)));
    m[1][2] = 3.0 * (tension + 2.0 * b2);
    m[2][0] = -6.0 * b3;
    m[2][1] = 6.0 * (b3 - bias);
    m[2][2] = 6.0 * bias;
    m[3][0] = 2.0 * b3;
    m[3][1] = tension + (4.0 * (b2 + bias));
    m[0][3] = m[3][2] = 2.0;
    m[1][3] = m[2][3] = m[3][3] = 0.0;
    for (auto& row : m)
        for (auto& value : row) value *= d;
    return m;
}

// CSpline::GetKnot closed case (spline.cpp:232) for a closed beta spline.
[[nodiscard]] auto get_knot(const std::vector<BalloonPoint>& cps, const int index) -> BalloonPoint {
    const int n = static_cast<int>(cps.size());
    if (index == 0) return cps[static_cast<std::size_t>(n - 1)];
    if (index == n + 1) return cps[0];
    if (index == n + 2) return cps[1];
    return cps[static_cast<std::size_t>(index - 1)];
}

[[nodiscard]] auto mat_apply(const Matrix& m, const int row, const BalloonPoint k0, const BalloonPoint k1,
                             const BalloonPoint k2, const BalloonPoint k3) -> BalloonPoint {
    return {
        iround(m[row][0] * k0.x + m[row][1] * k1.x + m[row][2] * k2.x + m[row][3] * k3.x),
        iround(m[row][0] * k0.y + m[row][1] * k1.y + m[row][2] * k2.y + m[row][3] * k3.y),
    };
}

} // namespace

auto beta_closed_bezier(const std::vector<BalloonPoint>& cps) -> std::vector<BalloonPoint> {
    const int n_cps = static_cast<int>(cps.size());
    if (n_cps < 2) return {};
    const Matrix m = beta_matrix(beta_default_tension, beta_default_bias);

    const int n_knots = n_cps + 3;             // CBeta::KnotCount, closed (spline.h:55)
    const int bezier_count = 3 * n_knots - 8;  // CSpline::BezierCount (spline.h:17)
    std::vector<BalloonPoint> bez(static_cast<std::size_t>(bezier_count));

    int bez_index = 1;
    BalloonPoint knot0 = get_knot(cps, 0);
    BalloonPoint knot1 = get_knot(cps, 1);
    BalloonPoint knot2 = get_knot(cps, 2);
    BalloonPoint knot3 = get_knot(cps, 3);
    for (int i = 0;; ++i) {
        // CvertsToCubic: c3<-row0, c2<-row1, c1<-row2, c0<-row3 (spline.cpp:206).
        const BalloonPoint c3 = mat_apply(m, 0, knot0, knot1, knot2, knot3);
        const BalloonPoint c2 = mat_apply(m, 1, knot0, knot1, knot2, knot3);
        const BalloonPoint c1 = mat_apply(m, 2, knot0, knot1, knot2, knot3);
        const BalloonPoint c0 = mat_apply(m, 3, knot0, knot1, knot2, knot3);
        // CubicToBezier (spline.cpp:218).
        const BalloonPoint b0 = c0;
        BalloonPoint b1{c0.x + iround((1.0 / 3.0) * c1.x), c0.y + iround((1.0 / 3.0) * c1.y)};
        BalloonPoint b2{b1.x + iround((1.0 / 3.0) * (c1.x + c2.x)), b1.y + iround((1.0 / 3.0) * (c1.y + c2.y))};
        const BalloonPoint b3{c0.x + c1.x + c2.x + c3.x, c0.y + c1.y + c2.y + c3.y};
        if (i == 0) bez[0] = b0;
        bez[static_cast<std::size_t>(bez_index)] = b1;
        bez[static_cast<std::size_t>(bez_index + 1)] = b2;
        bez[static_cast<std::size_t>(bez_index + 2)] = b3;
        if (i + 4 == n_knots) break;
        bez_index += 3;
        knot0 = knot1;
        knot1 = knot2;
        knot2 = knot3;
        knot3 = get_knot(cps, i + 4);
    }
    return bez;
}

auto cloud_bbox(const std::vector<BalloonPoint>& cps) -> Rect {
    constexpr int large = 100000000;
    Rect box{large, large, -large, -large};  // left, bottom, right, top
    for (const auto& pt : cps) {
        box.left = std::min(pt.x, box.left);
        box.bottom = std::min(pt.y, box.bottom);
        box.right = std::max(pt.x, box.right);
        box.top = std::max(pt.y, box.top);
    }
    return box;
}

auto dock_at_top(const Rect& bbox, const int height) -> Rect {
    const int old_height = bbox.top - bbox.bottom;
    Rect docked = bbox;
    docked.top = height + balloon_topborder;
    docked.bottom = docked.top - old_height;
    return docked;
}

auto area_estimate(const int text_extent, const int text_height, const int line_height) noexcept -> int {
    return static_cast<int>(1.3 * text_extent * (text_height + line_height));
}

auto cloud_estimate(MsvcrtRandom& rng, const CloudEstimateInput& in) -> CloudEstimate {
    const int area = area_estimate(in.text_extent, in.text_height, in.line_height);
    const int max_width = in.free_right - in.free_left;
    int goal_width = 0;

    if (in.text_extent <= one_line_threshold) {
        goal_width = in.text_extent;
    } else {
        const int potential_height = in.lowest_prev_bottom - in.free_bottom + min_hook_height;
        int min_width = potential_height != 0 ? area / potential_height : area;
        min_width = std::max(min_width, in.widest_word);
        goal_width = min_width + static_cast<int>(rng.next_float() * (max_width - min_width));
    }

    goal_width = std::min(goal_width + 200, max_width);
    goal_width = std::min(goal_width, in.text_extent + 200);

    int left = 0;
    if (in.is_box) {
        left = in.free_left;
    } else {
        const int left_limit = in.arrow_x - goal_width;
        const int right_limit = in.arrow_x;
        int start_x = left_limit + static_cast<int>(rng.next_float() * (right_limit - left_limit));
        if (start_x < in.free_left) start_x = in.free_left;
        if (start_x + goal_width > in.free_right) start_x = in.free_right - goal_width;
        left = start_x;
    }
    return {goal_width, left, left + goal_width};
}

auto compute_tail(const TailInput& in) -> TailGeometry {
    // AddArrow (balloon.cpp:1538) up to BreakSpline. All arithmetic in the
    // source's mixed panel/balloon coordinates.
    BalloonPoint bottom2{in.arrow_x, in.speaker_top + 200};

    int xbreak = ((in.route_left + in.route_right) / 2) - in.bbox_left;
    const int bottom_start = in.last_line_left;
    const int bottom_end = bottom_start + in.last_line_width;

    if (xbreak < bottom_start && bottom_start + in.bbox_left < in.route_right - balloon_large_delta) {
        xbreak = bottom_start + balloon_small_delta;
    } else if (xbreak > bottom_end && bottom_end + in.bbox_left > in.route_left + balloon_large_delta) {
        xbreak = bottom_end - balloon_small_delta;
    }

    BalloonPoint top2{xbreak + in.bbox_left, in.cloud_bottom};

    if (top2.y - bottom2.y < balloon_min_tail_height) {  // ensure minimum tail height
        bottom2.y = top2.y - balloon_min_tail_height;
    }

    double angle = vector_to_angle(sub(top2, bottom2));
    if (std::abs(angle) - pi / 2.0 > pi / 4.0) {  // clamp to <= 45 deg from vertical
        angle = (angle > 3.0 * pi / 4.0) ? 3.0 * pi / 4.0 : pi / 4.0;
        const int height_delta = top2.y - bottom2.y;
        xbreak = static_cast<int>(std::cos(angle) * height_delta + bottom2.x - in.bbox_left);
        top2.x = xbreak + in.bbox_left;
    }

    return {bottom2, top2, xbreak, angle};
}

auto think_bubbles(const BalloonPoint& entry, const BalloonPoint& tail) -> std::vector<ThinkBubble> {
    const int delta_y = entry.y - tail.y;
    if (delta_y < 0) return {};
    const int n_bubbles = (delta_y + balloon_inter_bubble) / (balloon_bubble_height + balloon_inter_bubble);
    if (n_bubbles <= 0) return {};

    const int bubble_spacing =
        (n_bubbles > 1) ? (delta_y - balloon_bubble_height * n_bubbles) / (n_bubbles - 1) : 0;
    const DPoint delta_vec{static_cast<double>(entry.x - tail.x), static_cast<double>(entry.y - tail.y)};
    const DPoint delta_norm = norm(delta_vec);
    BalloonPoint start = add(tail, to_i(scal(balloon_bubble_height / 2.0, delta_norm)));
    const BalloonPoint increment = to_i(scal(static_cast<double>(balloon_bubble_height) + bubble_spacing, delta_norm));
    const int width_delta =
        (n_bubbles > 1) ? (balloon_end_bubble_width - balloon_bubble_height) / (2 * (n_bubbles - 1)) : 0;

    std::vector<ThinkBubble> bubbles;
    bubbles.reserve(static_cast<std::size_t>(n_bubbles));
    int width_pad = 0;
    for (int i = 0; i < n_bubbles; ++i) {
        bubbles.push_back(ThinkBubble{start, balloon_bubble_height / 2, width_pad});
        start = add(start, increment);
        width_pad += width_delta;
    }
    return bubbles;
}

auto box_cloud_bbox(const Rect& text_bbox) -> Rect {
    return {
        text_bbox.left - balloon_xbox_delta,
        text_bbox.bottom - balloon_ybox_delta,
        text_bbox.right + balloon_xbox_delta,
        text_bbox.top + balloon_ybox_delta,
    };
}

auto box_outline(const Rect& text_bbox) -> std::vector<BalloonPoint> {
    const BalloonPoint pt1{text_bbox.left - balloon_xbox_delta, text_bbox.bottom - balloon_ybox_delta};
    const BalloonPoint pt2{pt1.x, text_bbox.top + balloon_ybox_delta};
    const BalloonPoint pt3{text_bbox.right + balloon_xbox_delta, pt2.y};
    const BalloonPoint pt4{pt3.x, pt1.y};
    return {pt1, pt2, pt3, pt4};
}

namespace {

[[nodiscard]] auto translate(std::vector<BalloonPoint> pts, const int dx, const int dy)
    -> std::vector<BalloonPoint> {
    for (auto& pt : pts) {
        pt.x += dx;
        pt.y += dy;
    }
    return pts;
}

} // namespace

auto layout_balloon(const BalloonRequest& request) -> Balloon {
    Balloon out;
    out.kind = request.kind;
    out.text = request.text;
    out.lines = request.lines;
    out.line_height = request.font.line_height;

    const int n_lines = static_cast<int>(request.lines.size());
    const int max_width = widest_line_width(request.lines);

    if (request.kind.mode == BalloonMode::action) {
        // Left-justified text bbox grown into the box (balloon.cpp:2042). The
        // text bbox has Top = 0, Bottom = -n*line_height - base_add
        // (balloon.cpp:711); Left = 0, Right = widest line (FT_LEFT_JUSTIFY).
        const Rect text_bbox{0, -n_lines * request.font.line_height - request.font.base_add, max_width, 0};
        const Rect true_box = box_cloud_bbox(text_bbox);
        const int bbox_left = request.place_left - true_box.left;
        int bbox_top = request.place_top - true_box.top;
        const int height = true_box.top - true_box.bottom;
        if (bbox_top > -250) bbox_top = request.place_top + balloon_topborder;  // DockAtTop
        out.bbox = {bbox_left, bbox_top - height, bbox_left + (true_box.right - true_box.left), bbox_top};
        out.outline = translate(box_outline(text_bbox), bbox_left, bbox_top);
        out.route_region = {true_box.left + bbox_left, true_box.bottom + bbox_top,
                            true_box.right + bbox_left, true_box.top + bbox_top};
        out.has_tail = false;
        return out;
    }

    const auto justify = LabelJustify::center;
    const auto offsets = shift_line_offsets(request.lines, justify);
    auto filters = get_filters(request.lines, offsets);
    const int final_y = permute_filters(filters, request.font);
    const auto local_spline = create_balloon_spline(filters, final_y);
    const Rect true_box = cloud_bbox(local_spline);

    // SetBBox origin (balloon.cpp:1486) then DockAtTop (balloon.cpp:1306) when
    // the cloud sits near the top.
    int bbox_left = request.place_left - true_box.left;
    int bbox_top = request.place_top - true_box.top;
    const int height = true_box.top - true_box.bottom;
    if (bbox_top > -250) {
        bbox_top = request.place_top + balloon_topborder;  // DockAtTop
    }
    out.bbox = {bbox_left, bbox_top - height, bbox_left + (true_box.right - true_box.left), bbox_top};

    out.spline = translate(local_spline, bbox_left, bbox_top);
    out.outline = beta_closed_bezier(out.spline);
    out.route_region = {true_box.left + bbox_left, true_box.bottom + bbox_top,
                        true_box.right + bbox_left, true_box.top + bbox_top};
    out.has_tail = true;

    const int last_left = offsets.empty() ? 0 : offsets.back();
    const int last_width = request.lines.empty() ? 0 : request.lines.back().width;
    out.tail = compute_tail(TailInput{
        request.arrow_x, request.speaker_top, bbox_left, bbox_top, out.route_region.left,
        out.route_region.right, out.route_region.bottom, last_left, last_width});

    if (request.kind.mode == BalloonMode::think) {
        // CBWoodringThink::Draw (balloon.cpp:1970): the bubble entry X centers on
        // the cloud route region, but the entry Y is the TEXT bbox bottom
        // (fInfo.m_bbox.Bottom = -(nLines*lineHeight + baseAdd)), not the cloud
        // bbox bottom -- the cloud bottom sits lower by the AddWavies scallops and
        // the finalY inset, which would change the bubble count and spacing.
        const int text_bbox_bottom =
            bbox_top - n_lines * request.font.line_height - request.font.base_add;
        const BalloonPoint entry{(out.route_region.left + out.route_region.right) / 2,
                                 text_bbox_bottom};
        const BalloonPoint tail{request.arrow_x, request.speaker_top + 200};
        out.bubbles = think_bubbles(entry, tail);
        out.has_tail = false;  // think replaces the tail with the bubble trail
    }
    return out;
}

} // namespace comicchat
