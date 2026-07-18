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

// CSpline::GetKnot (spline.cpp:232). `closed` selects the wrap model: the closed
// branch duplicates cps[n-1]/cps[0]/cps[1] around the seam; the OPEN branch
// (spline.cpp:241-248) duplicates the first control point for the first `dups`(=3)
// knots and the last control point for the trailing knots, so an open beta spline
// starts/ends near cps[0]/cps[n-1] instead of looping.
[[nodiscard]] auto get_knot(const std::vector<BalloonPoint>& cps, const int index, const bool closed)
    -> BalloonPoint {
    const int n = static_cast<int>(cps.size());
    if (closed) {
        if (index == 0) return cps[static_cast<std::size_t>(n - 1)];
        if (index == n + 1) return cps[0];
        if (index == n + 2) return cps[1];
        return cps[static_cast<std::size_t>(index - 1)];
    }
    constexpr int dups = 3;  // CBeta::GetDups (spline.h:54)
    if (index < dups) return cps[0];
    if (index >= n + dups - 2) return cps[static_cast<std::size_t>(n - 1)];
    return cps[static_cast<std::size_t>(index - dups + 1)];
}

[[nodiscard]] auto mat_apply(const Matrix& m, const int row, const BalloonPoint k0, const BalloonPoint k1,
                             const BalloonPoint k2, const BalloonPoint k3) -> BalloonPoint {
    return {
        iround(m[row][0] * k0.x + m[row][1] * k1.x + m[row][2] * k2.x + m[row][3] * k3.x),
        iround(m[row][0] * k0.y + m[row][1] * k1.y + m[row][2] * k2.y + m[row][3] * k3.y),
    };
}

// CSpline::ComputeBezpts (spline.cpp:169): expand the beta control points into
// the PolyBezier control-point list. `closed` picks the knot model and counts:
// closed -> KnotCount nCps+3, BezierCount 3*nCps+1; open -> KnotCount nCps+4,
// BezierCount 3*nCps+4. The loop body is identical for both (the only difference
// is get_knot's wrap and the terminating knot count).
[[nodiscard]] auto beta_bezier(const std::vector<BalloonPoint>& cps, const bool closed)
    -> std::vector<BalloonPoint> {
    const int n_cps = static_cast<int>(cps.size());
    if (n_cps < 2) return {};
    const Matrix m = beta_matrix(beta_default_tension, beta_default_bias);

    const int n_knots = closed ? n_cps + 3 : n_cps + 4;  // CBeta::KnotCount (spline.h:55)
    const int bezier_count = 3 * n_knots - 8;             // CSpline::BezierCount (spline.h:17)
    std::vector<BalloonPoint> bez(static_cast<std::size_t>(bezier_count));

    int bez_index = 1;
    BalloonPoint knot0 = get_knot(cps, 0, closed);
    BalloonPoint knot1 = get_knot(cps, 1, closed);
    BalloonPoint knot2 = get_knot(cps, 2, closed);
    BalloonPoint knot3 = get_knot(cps, 3, closed);
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
        knot3 = get_knot(cps, i + 4, closed);
    }
    return bez;
}

} // namespace

auto beta_closed_bezier(const std::vector<BalloonPoint>& cps) -> std::vector<BalloonPoint> {
    return beta_bezier(cps, /*closed=*/true);
}

auto beta_open_bezier(const std::vector<BalloonPoint>& cps) -> std::vector<BalloonPoint> {
    return beta_bezier(cps, /*closed=*/false);
}

namespace {

// ------------------------------------------------------------------------
// splinutl.cpp double-precision bezier walk (ported for BreakSpline). A cubic
// bezier is recursively split until flat, then sampled ~epsilon apart, running a
// callback at each sample. Two callbacks are needed: nearest-point (ClosestPoint)
// and beyond-goal-x (WalkHorizontalDistance).
// ------------------------------------------------------------------------
constexpr double spline_epsilon = 1.0;    // splinutl.cpp:57
constexpr double spline_small_number = 1.0e-24;  // vector2d.h:26

struct DBezier final {
    DPoint p0, p1, p2, p3;
};

[[nodiscard]] auto dadd(const DPoint a, const DPoint b) noexcept -> DPoint { return {a.x + b.x, a.y + b.y}; }
[[nodiscard]] auto dsub(const DPoint a, const DPoint b) noexcept -> DPoint { return {a.x - b.x, a.y - b.y}; }
[[nodiscard]] auto ddist(const DPoint a, const DPoint b) noexcept -> double {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

// split_bezier (splinutl.cpp:20): de Casteljau split at t = 1/2.
void split_bezier(const DBezier& b, DBezier& left, DBezier& right) {
    left.p0 = b.p0;
    left.p1 = scal(0.5, dadd(b.p0, b.p1));
    const DPoint t = scal(0.5, dadd(b.p1, b.p2));
    left.p2 = scal(0.5, dadd(left.p1, t));
    right.p3 = b.p3;
    right.p2 = scal(0.5, dadd(b.p2, b.p3));
    right.p1 = scal(0.5, dadd(t, right.p2));
    left.p3 = right.p0 = scal(0.5, dadd(left.p2, right.p1));
}

[[nodiscard]] auto inside_bbox_tol(const DPoint pt, const double xmin, const double xmax, const double ymin,
                                   const double ymax, const double tol) noexcept -> bool {
    return !((pt.x + tol < xmin) || (pt.x - tol > xmax) || (pt.y + tol < ymin) || (pt.y - tol > ymax));
}

// flat_bezier (splinutl.cpp:58): approximate flatness test against epsilon.
[[nodiscard]] auto flat_bezier(const DBezier& b) noexcept -> bool {
    const double xmin = std::min(b.p0.x, b.p3.x);
    const double xmax = std::max(b.p0.x, b.p3.x);
    const double ymin = std::min(b.p0.y, b.p3.y);
    const double ymax = std::max(b.p0.y, b.p3.y);
    if (!inside_bbox_tol(b.p1, xmin, xmax, ymin, ymax, 0.5 * spline_epsilon) ||
        !inside_bbox_tol(b.p2, xmin, xmax, ymin, ymax, 0.5 * spline_epsilon))
        return false;

    const DPoint d1 = dsub(b.p1, b.p0);
    const DPoint d2 = dsub(b.p2, b.p0);
    const DPoint d = dsub(b.p3, b.p0);
    const double dx = std::abs(d.x);
    const double dy = std::abs(d.y);
    if (dx + dy < spline_epsilon) return true;
    if (dy < dx) {
        const double dydx = d.y / d.x;
        return std::abs(d2.y - (d2.x * dydx)) < spline_epsilon &&
               std::abs(d1.y - (d1.x * dydx)) < spline_epsilon;
    }
    const double dxdy = d.x / d.y;
    return std::abs(d2.x - (d2.y * dxdy)) < spline_epsilon &&
           std::abs(d1.x - (d1.y * dxdy)) < spline_epsilon;
}

// subdivide (splinutl.cpp:92): walk the flattened bezier, running `proc` roughly
// `spline_epsilon` apart. `proc` returns true to stop. Depth is bounded because
// flat_bezier converges just as the source relies on.
template <typename Proc>
[[nodiscard]] auto subdivide(const DBezier& bezier, Proc&& proc) -> bool {
    if (flat_bezier(bezier)) {
        const double length = ddist(bezier.p0, bezier.p3);
        if (length > spline_small_number) {
            const double step = spline_epsilon / length;
            for (double alpha = 0.0; alpha <= 1.0; alpha += step) {
                const DPoint pt = dadd(scal(alpha, bezier.p3), scal(1.0 - alpha, bezier.p0));
                if (proc(pt)) return true;
            }
        }
        return proc(bezier.p3);
    }
    DBezier left{};
    DBezier right{};
    split_bezier(bezier, left, right);
    if (subdivide(left, proc)) return true;
    return subdivide(right, proc);
}

[[nodiscard]] auto to_dbezier(const BalloonPoint* triple) -> DBezier {
    return {to_d(triple[0]), to_d(triple[1]), to_d(triple[2]), to_d(triple[3])};
}

// int_bezier_nearest_point (splinutl.cpp:184): manhattan-nearest point on one
// cubic. `dist` truncates like the source (int) cast.
struct NearestResult final {
    int dist{};
    BalloonPoint found{};
};
[[nodiscard]] auto bezier_nearest(const BalloonPoint* triple, const BalloonPoint given) -> NearestResult {
    const DBezier b = to_dbezier(triple);
    const DPoint given_d = to_d(given);
    double best = 1.0e24;  // LARGENUMBER (vector2d.h:25)
    DPoint found{};
    // cb_nearest never stops the walk (it always returns FALSE); the whole cubic
    // is sampled, so the walk's own return is intentionally unused (splinutl.cpp:206).
    (void)subdivide(b, [&](const DPoint pt) {
        const double this_dist = std::abs(pt.x - given_d.x) + std::abs(pt.y - given_d.y);
        if (this_dist < best) {
            best = this_dist;
            found = pt;
        }
        return false;
    });
    return {static_cast<int>(best), {static_cast<int>(found.x), static_cast<int>(found.y)}};
}

// walk_horizontal_dist (splinutl.cpp:259): the rightmost sample on one cubic, and
// whether any sample reached goal_x. found.x starts below any real sample.
struct HorizontalResult final {
    bool found{};
    BalloonPoint furthest{};
};
[[nodiscard]] auto bezier_walk_horizontal(const BalloonPoint* triple, const int goal_x) -> HorizontalResult {
    const DBezier b = to_dbezier(triple);
    DPoint best{-1000000.0, 0.0};
    const bool found = subdivide(b, [&](const DPoint pt) {
        if (pt.x > best.x) best = pt;
        return pt.x >= static_cast<double>(goal_x);
    });
    return {found, to_i(best)};  // dpoint_to_point ROUNDs (vector2d.cpp:159)
}

struct ClosestResult final {
    BalloonPoint point{};
    int knot_index{};
};

// CSpline::ClosestPoint (spline.cpp:251): manhattan-nearest point across every
// cubic of the CLOSED bezier, plus the knot index (i/3 + 2) it landed on.
[[nodiscard]] auto spline_closest_point(const std::vector<BalloonPoint>& bez, const BalloonPoint to_pt)
    -> ClosestResult {
    const int bez_count = static_cast<int>(bez.size());
    int min_dist = 10000000;
    ClosestResult result{};
    for (int i = 0; i < bez_count - 1; i += 3) {
        const NearestResult near = bezier_nearest(bez.data() + i, to_pt);
        if (near.dist < min_dist) {
            min_dist = near.dist;
            result.point = near.found;
            result.knot_index = (i / 3) + 2;
        }
    }
    return result;
}

// CSpline::WalkHorizontalDistance (spline.cpp:269): from `from_knot_index`, walk
// forward around the CLOSED bezier until a sample's x reaches `goal_x`, else keep
// the rightmost sample seen. Returns the point and the knot index it came from.
[[nodiscard]] auto spline_walk_horizontal(const std::vector<BalloonPoint>& bez, const int from_knot_index,
                                          const int goal_x) -> ClosestResult {
    const int bez_count = static_cast<int>(bez.size());
    ClosestResult result{};
    result.knot_index = -1;
    int index = (from_knot_index - 2) * 3;
    BalloonPoint last_furthest{-100000, -100000};
    for (int i = 0; i < bez_count - 1; i += 3) {
        if (index + 3 > bez_count - 1) index = 0;
        const HorizontalResult walk = bezier_walk_horizontal(bez.data() + index, goal_x);
        if (walk.found) {
            result.knot_index = index / 3 + 2;
            result.point = walk.furthest;
            return result;
        }
        if (walk.furthest.x > last_furthest.x) {
            result.knot_index = index / 3 + 2;
            last_furthest = walk.furthest;
        }
        index += 3;
    }
    result.point = last_furthest;
    return result;
}

} // namespace

auto break_spline_open(const std::vector<BalloonPoint>& spline_cps,
                       const std::vector<BalloonPoint>& closed_outline, const int x_panel,
                       const int y_panel) -> BrokenCloud {
    const int n_cps = static_cast<int>(spline_cps.size());
    if (n_cps < 2 || closed_outline.size() < 4) return {};

    constexpr int gapwidth = 80;  // BreakSpline gapwidth (balloon.cpp:457)
    const BalloonPoint left{x_panel - gapwidth, y_panel};

    const ClosestResult left_hit = spline_closest_point(closed_outline, left);
    const ClosestResult right_hit =
        spline_walk_horizontal(closed_outline, left_hit.knot_index, left_hit.point.x + 2 * gapwidth);
    const int left_knot = left_hit.knot_index;
    const int right_knot = right_hit.knot_index;
    if (left_knot < 0 || right_knot < 0) return {};

    // Rebuild the control array (balloon.cpp:464-470): start at rightNearest, walk
    // the surviving control points forward from (right_knot-1), end at leftNearest.
    // The (right_knot-left_knot) mod nCps points spanning the tail gap are dropped.
    const int n_new = n_cps + 2 - (right_knot - left_knot + n_cps) % n_cps;
    std::vector<BalloonPoint> new_cps(static_cast<std::size_t>(n_new));
    new_cps[0] = right_hit.point;
    for (int i = 1; i <= n_new - 2; ++i) {
        const int src = ((right_knot + i - 2) % n_cps + n_cps) % n_cps;
        new_cps[static_cast<std::size_t>(i)] = spline_cps[static_cast<std::size_t>(src)];
    }
    new_cps[static_cast<std::size_t>(n_new - 1)] = left_hit.point;

    BrokenCloud broken;
    broken.outline_open = beta_open_bezier(new_cps);
    broken.gap_left = left_hit.point;    // leftNearest = cps[nCpsNew-1]
    broken.gap_right = right_hit.point;  // rightNearest = cps[0]
    return broken;
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

    // fInfo.m_bbox.Bottom (balloon.cpp:711) lifted into panel space: the TEXT bbox
    // bottom, one AddWavies scallop above the cloud bottom. BreakSpline breaks on
    // this row (balloon.cpp:1585); the think-bubble entry uses the same value.
    const int text_bbox_bottom = bbox_top - n_lines * request.font.line_height - request.font.base_add;

    // SetBalloonTraj (balloon.cpp:1886): one CTraj = the cloud spline + AddArrow.
    // AddArrow calls BreakSpline to open the cloud bottom at the tail throat, then
    // AddSeg's two CArc tail edges bridging the gap endpoints -- a single open
    // cloud+tail figure with no stroked-across seam. think inherits this traj
    // (CBWoodringThink overrides only Draw, balloon.cpp:1966), so it too carries
    // the pointed tail beneath its bubble trail.
    const auto broken = break_spline_open(out.spline, out.outline, out.tail.tip.x, text_bbox_bottom);
    out.outline_open = broken.outline_open;
    out.tail_gap_left = broken.gap_left;
    out.tail_gap_right = broken.gap_right;

    if (!out.outline_open.empty()) {
        // AddArrow (balloon.cpp:1588-1601): top2 recomputed as the gap-endpoint
        // midpoint, tailLen = dist(top2, bottom2), alt = 0.05*tailLen, and
        // sign = (bottom.x > left.x ? 1 : -1). The two edges bow +/-alt, apart.
        out.tail.tip = {(broken.gap_left.x + broken.gap_right.x) / 2,
                        (broken.gap_left.y + broken.gap_right.y) / 2};
        const double tail_len = dist(out.tail.tip, out.tail.anchor);
        out.tail.altitude = static_cast<int>(0.05 * tail_len);
        out.tail.tail_sign = out.tail.anchor.x > broken.gap_left.x ? 1 : -1;
    }

    if (request.kind.mode == BalloonMode::think) {
        // CBWoodringThink::Draw (balloon.cpp:1970): the bubble entry X centers on
        // the cloud route region, but the entry Y is the TEXT bbox bottom
        // (fInfo.m_bbox.Bottom = -(nLines*lineHeight + baseAdd)), not the cloud
        // bbox bottom -- the cloud bottom sits lower by the AddWavies scallops and
        // the finalY inset, which would change the bubble count and spacing.
        const BalloonPoint entry{(out.route_region.left + out.route_region.right) / 2,
                                 text_bbox_bottom};
        const BalloonPoint tail{request.arrow_x, request.speaker_top + 200};
        out.bubbles = think_bubbles(entry, tail);
        // think keeps BOTH the pointed tail (open cloud + bowed arcs) AND the
        // bubble trail -- it does not replace one with the other (balloon.cpp:1966).
    }
    return out;
}

} // namespace comicchat
