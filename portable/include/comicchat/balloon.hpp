#pragma once

#include "comicchat/cpp26.hpp"
#include "comicchat/layout.hpp"
#include "comicchat/text.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Item 2.1 — comic balloon geometry, tails, thought/whisper/box shapes
// (render-port-spec.md §2.1). This is the renderer capstone: it ports the
// LayoutBalloons/LayoutBalloon/GetCloudEstimate/DockAtTop flow (panel.cpp), the
// CBeta beta-spline cloud outline (CreateBalloonSpline/PermuteFilters/AddWavies,
// balloon.cpp:1839/531/563), the AddArrow tail (balloon.cpp:1538), and the
// whisper (dashed) / think (bubbles) / action-box variants
// (balloon.cpp:1919/1966/2011).
//
// COORDINATE MODEL. Everything here is in the panel-local logical drawing space
// of render-port-spec.md §0: Win32 MM_TWIPS, Y-up, panel interior from top = 0
// down to bottom = -m_unitHeight. Points keep the source POINT (integer) type
// and its ROUND/truncation semantics so control-point sequences reproduce
// byte-for-byte. render_panel applies the single device transform
// (translate(origin)·scale(scale, -scale)) exactly as fill_logical_rect does.
//
// HONEST LIMIT. The control-point / bezier-expansion / wavy-permutation / tail
// arithmetic and shape selection are deterministic integer math and are proven
// on Linux (see render_test.cpp). Exact GDI-PolyBezier-vs-Cairo-curve_to cloud
// PIXELS are only checkable visually / under MSVC and are NOT claimed here.

namespace comicchat {

// ------------------------------------------------------------------------
// balloon.cpp constants (balloon.cpp:54-77, 99).
// ------------------------------------------------------------------------
inline constexpr int balloon_xborder = 100;         // XBORDER
inline constexpr int balloon_yborder = 40;          // YBORDER
inline constexpr int balloon_topborder = -20;       // TOPBORDER
inline constexpr int balloon_hwave_height = 70;      // HWAVEHEIGHT
inline constexpr int balloon_hwave_interval = 300;   // HWAVEINTERVAL
inline constexpr int balloon_vwave_height = 70;      // VWAVEHEIGHT
inline constexpr int balloon_vwave_interval = 300;   // VWAVEINTERVAL
inline constexpr int balloon_thresh1 = -70;          // THRESH1
inline constexpr int balloon_thresh2 = 70;           // THRESH2
inline constexpr int balloon_max_pts = 150;          // MAXPTS
inline constexpr int balloon_min_tail_height = 100;  // MINTAILHEIGHT
inline constexpr int balloon_min_route_width = 300;  // MINROUTEWIDTH
inline constexpr int balloon_large_delta = 350;      // LARGEDELTA
inline constexpr int balloon_small_delta = 150;      // SMALLDELTA
inline constexpr int balloon_xbox_delta = 90;        // XBOXDELTA
inline constexpr int balloon_ybox_delta = 50;        // YBOXDELTA
inline constexpr int balloon_bubble_height = 150;    // BUBBLEHEIGHT
inline constexpr int balloon_inter_bubble = 100;     // INTERBUBBLE
inline constexpr int balloon_end_bubble_width = 400; // ENDBUBBLEWIDTH

// CBeta::defaultTension / defaultBias (spline.cpp:65,66).
inline constexpr double beta_default_tension = 5.0;
inline constexpr double beta_default_bias = 1.0;

// panel.cpp balloon-layout constants (panel.cpp:40,41).
inline constexpr int one_line_threshold = 500;  // ONELINETHRESHOLD
inline constexpr int min_hook_height = 100;      // MINHOOKHEIGHT

// MakeBalloon subclass selection (panel.cpp:1039).
enum class BalloonMode : std::uint8_t { say, whisper, think, action };

// The uModes bitfield the source AddLine/MakeBalloon consumes (chat.h BM_*),
// reproduced so select_balloon_mode maps raw bits exactly as panel.cpp:1041-1057.
inline constexpr std::uint16_t bm_say = 0x0001;
inline constexpr std::uint16_t bm_think = 0x0002;
inline constexpr std::uint16_t bm_whisper = 0x0004;
inline constexpr std::uint16_t bm_action = 0x0008;

// The concrete balloon subclass MakeBalloon (panel.cpp:1039) picks from uModes:
// BM_SAY->normal, BM_WHISPER->dashed whisper, BM_THINK->think, any BM_ACTION*->
// box (dashed when BM_WHISPER is also set, panel.cpp:1053).
struct BalloonShapeKind final {
    BalloonMode mode{BalloonMode::say};
    bool dashed{};
    auto operator==(const BalloonShapeKind&) const -> bool = default;
};
[[nodiscard]] auto select_balloon_mode(std::uint16_t modes) noexcept -> BalloonShapeKind;

// ------------------------------------------------------------------------
// Geometry primitives.
// ------------------------------------------------------------------------

// The source Win32 POINT (integer x/y). Kept integer with ROUND/truncation
// semantics so ported control-point streams are byte-identical.
struct BalloonPoint final {
    int x{};
    int y{};
    auto operator==(const BalloonPoint&) const -> bool = default;
};

// RANGE (balloon.cpp:101): one run of the left/right text staircase. `start`/
// `end` are the inclusive line indices; `x` is the run's binding edge; `y` is
// the vertical the run's corner sits at after PermuteFilters.
struct FilterRun final {
    int start{};
    int end{};
    int x{};
    int y{};
    auto operator==(const FilterRun&) const -> bool = default;
};

struct FilterStaircases final {
    std::vector<FilterRun> left;
    std::vector<FilterRun> right;
    auto operator==(const FilterStaircases&) const -> bool = default;
};

// ShiftLines (balloon.cpp:768): the per-line left offset. MAXLEFTSHIFT /
// MAXCENTERSHIFT are 0 in the shipped source (balloon.cpp:72,73), so the random
// jitter is always 0 — a center line offsets to (max_width - width)/2, a
// left-justified line to 0. (The source still draws one randfloat() per line;
// callers that need the exact multi-balloon rand() stream must advance the PRNG
// accordingly, but the resulting offset is deterministic and shift-free.)
[[nodiscard]] auto shift_line_offsets(const std::vector<TextLine>& lines, LabelJustify justify)
    -> std::vector<int>;

// GetFilters (balloon.cpp:482): turn the per-line left x + width into left and
// right staircases (RANGE runs) using the THRESH1/THRESH2 step thresholds, so a
// tightly-binding cloud follows ragged text. `left_x[i]` is line i's left edge
// (from shift_line_offsets); the line width comes from `lines[i].width`.
[[nodiscard]] auto get_filters(const std::vector<TextLine>& lines, const std::vector<int>& left_x)
    -> FilterStaircases;

// PermuteFilters (balloon.cpp:531): assign each staircase corner its Y from the
// font metrics (line_height / base_add / top_offset) and inset each x by
// ±XBORDER. Mutates `filters` in place and returns finalY (balloon.cpp:559), the
// vertical the bottom of the cloud closes at.
[[nodiscard]] auto permute_filters(FilterStaircases& filters, const FontMetrics& font) -> int;

// AddWavies (balloon.cpp:563): append the scalloped bumps between pt1 and pt2
// into `pts`. `wave_diam` is the perpendicular amplitude (HWAVEHEIGHT /
// VWAVEHEIGHT); `interval` the period. Fewer than two periods adds nothing.
void add_wavies(const BalloonPoint& pt1, const BalloonPoint& pt2, std::vector<BalloonPoint>& pts,
                int wave_diam, int interval);

// CreateBalloonSpline (balloon.cpp:1839): the full cloud control-point stream —
// down the left staircase, across the bottom, up the right staircase, closing
// across the top — with AddWavies inserted along every edge. `final_y` is the
// permute_filters return. This is the deterministic sequence goldened on Linux.
[[nodiscard]] auto create_balloon_spline(const FilterStaircases& filters, int final_y)
    -> std::vector<BalloonPoint>;

// CBeta(pts, n, TRUE) closed beta-spline expansion (spline.cpp:68,169): the
// Kochanek-Bartels/beta matrix (tension 5, bias 1) turned into the Bezier
// control-point list a PolyBezier draws (BezierCount() = 3*nCps + 1 points).
// Deterministic integer math (ROUND per coordinate); Linux-provable.
[[nodiscard]] auto beta_closed_bezier(const std::vector<BalloonPoint>& cps)
    -> std::vector<BalloonPoint>;

// CBeta(pts, n, FALSE) OPEN beta-spline expansion (spline.cpp:55,169,241): the
// same beta matrix, but the OPEN knot model (dups=3 duplicating the first/last
// control point, KnotCount = nCps + 4, BezierCount = 3*nCps + 4, GetKnot's open
// branch). This is the curve BreakSpline draws after it rewrites the control
// array to start/end at the two tail-gap points and clears `closed`
// (balloon.cpp:477-478). The path does NOT loop back to its start.
[[nodiscard]] auto beta_open_bezier(const std::vector<BalloonPoint>& cps)
    -> std::vector<BalloonPoint>;

// The result of BreakSpline (balloon.cpp:451-479): the cloud opened at the tail
// throat. `outline_open` is the OPEN beta bezier of the rewritten control array
// (running the long way round from `gap_right` over the cloud top to
// `gap_left`); `gap_left`/`gap_right` are the two real wavy-bottom points the
// tail arcs bridge (leftNearest/rightNearest, panel coords).
struct BrokenCloud final {
    std::vector<BalloonPoint> outline_open;
    BalloonPoint gap_left{};   // leftNearest = cps[nCpsNew-1]
    BalloonPoint gap_right{};  // rightNearest = cps[0]
};

// BreakSpline (balloon.cpp:451-479) in panel coordinates: open the CLOSED cloud
// (`spline_cps` control points, `closed_outline` its closed beta bezier) at the
// break column `x_panel` on the row `y_panel` (fInfo.m_bbox.Bottom lifted into
// panel space). Ports CSpline::ClosestPoint / WalkHorizontalDistance
// (spline.cpp:251-296) and the modular control-array rebuild (balloon.cpp:464-470).
[[nodiscard]] auto break_spline_open(const std::vector<BalloonPoint>& spline_cps,
                                     const std::vector<BalloonPoint>& closed_outline, int x_panel,
                                     int y_panel) -> BrokenCloud;

// ComputeCloudBBox (balloon.cpp:1504): the tight bounding box of the control
// points, as a twips/Y-up Rect (top > bottom).
[[nodiscard]] auto cloud_bbox(const std::vector<BalloonPoint>& cps) -> Rect;

// DockAtTop (balloon.cpp:1306): pin a near-top balloon so its top sits at
// `height + TOPBORDER`, preserving height. Returns the repositioned bbox.
[[nodiscard]] auto dock_at_top(const Rect& bbox, int height) -> Rect;

// ------------------------------------------------------------------------
// GetCloudEstimate (panel.cpp:888): the RNG-consuming goal-width + x placement.
// ------------------------------------------------------------------------
struct CloudEstimateInput final {
    int text_extent{};      // GetFormattedTextExtent cx (natural single-line width)
    int text_height{};      // GetFormattedTextExtent cy
    int line_height{};      // m_fontI->m_lineHeight
    int widest_word{};      // CLabel::WidestWord
    int free_left{};        // freeRect.left
    int free_right{};       // freeRect.right
    int free_top{};         // freeRect.top (0)
    int free_bottom{};      // freeRect.bottom (-m_unitHeight/2)
    int lowest_prev_bottom{};  // LowestPreviousBottom(...) (== free_top for balloon 0)
    int arrow_x{};          // m_speaker->m_arrowX
    bool is_box{};          // PE_BOX: left-align at free_left, no x jitter
};

struct CloudEstimate final {
    int goal_width{};   // brect width
    int left{};         // brect.left
    int right{};        // brect.right
    auto operator==(const CloudEstimate&) const -> bool = default;
};

// AreaEstimate (balloon.cpp:722): 1.3 * cx * (cy + line_height).
[[nodiscard]] auto area_estimate(int text_extent, int text_height, int line_height) noexcept -> int;

// Draw the goal width and x placement exactly in the source rand() order
// (panel.cpp:896-924): a one-liner keeps its natural length; otherwise a random
// width in [minWidth, maxWidth]; then a random overlap x around the speaker.
[[nodiscard]] auto cloud_estimate(MsvcrtRandom& rng, const CloudEstimateInput& in) -> CloudEstimate;

// ------------------------------------------------------------------------
// AddArrow tail (balloon.cpp:1538) — the deterministic anchor + break arithmetic.
// ------------------------------------------------------------------------
struct TailInput final {
    int arrow_x{};        // m_speaker->m_arrowX (panel coords)
    int speaker_top{};    // m_speaker->m_bbox.Top (panel coords, <= 0)
    int bbox_left{};      // balloon m_bbox.Left (panel offset)
    int bbox_top{};       // balloon m_bbox.Top (panel offset)
    int route_left{};     // m_routeRgn.Left (panel coords)
    int route_right{};    // m_routeRgn.Right (panel coords)
    int cloud_bottom{};   // GetCloudBBox().Bottom (panel coords)
    int last_line_left{}; // m_rgiLeftX[last] (balloon-local)
    int last_line_width{};// m_rgiWidths[last]
};

struct TailGeometry final {
    BalloonPoint anchor{};  // bottom2 = (arrow_x, speaker_top + 200), panel coords
    BalloonPoint tip{};     // top2: (xbreak + bbox_left, cloud_bottom) from compute_tail,
                            // updated by layout_balloon to the gap-endpoint midpoint
                            // after BreakSpline (balloon.cpp:1590-1591), panel coords
    int xbreak{};           // chosen break x, balloon-local (after text-nudge + angle clamp)
    double angle{};         // tail angle from vector_to_angle, clamped to <= 45 deg
    // AddArrow bow parameters (balloon.cpp:1595-1601). `altitude` = 0.05*tailLen,
    // the CArc bow height; `tail_sign` = (anchor.x > gap_left.x ? 1 : -1). The two
    // tail edges bow with +tail_sign*altitude and -tail_sign*altitude, curving
    // apart. Zero until layout_balloon runs BreakSpline (compute_tail leaves them 0).
    int altitude{};
    int tail_sign{1};
    auto operator==(const TailGeometry&) const -> bool = default;
};

// AddArrow's anchor/xbreak/angle math up to (but not including) BreakSpline: the
// tail bottom pins at (arrow_x, speaker_top + 200), xbreak starts at the route
// midpoint, is nudged under the last text line, the tail is forced to at least
// MINTAILHEIGHT, and the angle is clamped to 45 deg from vertical (recomputing
// xbreak). This is the deterministic part; the spline surgery + CArc curve are a
// render-time detail (honest limit: not pixel-goldened).
[[nodiscard]] auto compute_tail(const TailInput& in) -> TailGeometry;

// ------------------------------------------------------------------------
// Think bubbles (balloon.cpp:1966) and action box (balloon.cpp:2018,2042).
// ------------------------------------------------------------------------
struct ThinkBubble final {
    BalloonPoint center{};
    int radius{};      // BUBBLEHEIGHT/2
    int width_pad{};   // half-width growth toward the end bubble
    auto operator==(const ThinkBubble&) const -> bool = default;
};

// The shrinking-ellipse trail replacing a think balloon's tail (balloon.cpp:1979).
[[nodiscard]] auto think_bubbles(const BalloonPoint& entry, const BalloonPoint& tail)
    -> std::vector<ThinkBubble>;

// CBWoodringBox::ComputeCloudBBox (balloon.cpp:2042): the text bbox grown by
// XBOXDELTA/YBOXDELTA.
[[nodiscard]] auto box_cloud_bbox(const Rect& text_bbox) -> Rect;

// CBWoodringBox::SetBalloonTraj (balloon.cpp:2018): the four inset corners of
// the action-box rectangle, counter-clockwise from the bottom-left.
[[nodiscard]] auto box_outline(const Rect& text_bbox) -> std::vector<BalloonPoint>;

// ------------------------------------------------------------------------
// Assembled panel model (render-port-spec.md §2.1.f) consumed by render_panel.
// ------------------------------------------------------------------------

// A fully laid-out balloon in panel-local twips (Y-up), ready to draw.
struct Balloon final {
    BalloonShapeKind kind{};
    std::string text;
    std::vector<TextLine> lines;
    Rect bbox{};                             // m_bbox positioned in panel space
    Rect route_region{};                     // GetCloudBBox, panel coords
    std::vector<BalloonPoint> spline;        // cloud control points, panel coords
    std::vector<BalloonPoint> outline;       // CLOSED beta bezier expansion, panel coords
    // The OPEN cloud+tail figure (BreakSpline output, balloon.cpp:451-479): the
    // cloud broken at the tail throat so the bottom is never stroked across the
    // gap. render.cpp traces this WITHOUT a close_path, then bridges the gap with
    // the two bowed tail arcs and closes once -- a single seamless figure. Empty
    // for action boxes (which stay a closed rectangle with no tail).
    std::vector<BalloonPoint> outline_open;  // OPEN beta bezier, gap_right -> ... -> gap_left
    BalloonPoint tail_gap_left{};            // leftNearest on the wavy cloud bottom (panel)
    BalloonPoint tail_gap_right{};           // rightNearest on the wavy cloud bottom (panel)
    TailGeometry tail{};                     // say/whisper/think tail anchor + bow params
    std::vector<ThinkBubble> bubbles;        // think trail (empty otherwise)
    bool has_tail{};                         // false for action boxes
    int line_height{};                       // for text stacking
    // The font pixel size (in panel twips) the text was MEASURED at
    // (build_font_metrics / measure_text_width). render_panel draws each line at
    // this size * transform.scale, so the drawn glyphs are the same size the
    // cloud was fitted to and the text lands inside the outline. Set by the
    // page/comic_page assembly from message_text_size; 0 means "unset" and the
    // renderer falls back to a line-height estimate (synthetic-width demos only).
    double text_size{};
};

// One placed avatar body slot (from Item 2.3 placement + Item 2.2 geometry). The
// renderer draws a body placeholder here and the balloon points its tail at
// `arrow_x`.
struct PanelBody final {
    std::uint32_t avatar_id{};
    Rect box{};                 // body box, panel twips (Y-up)
    int arrow_x{};              // m_arrowX tail anchor
    std::uint32_t color{0x6c8ebfU};
    bool flip{};
};

// Panel { u32 seed; vector<Balloon>; vector<Body> } (render-port-spec.md §2.1.f).
struct Panel final {
    std::uint32_t seed{};
    std::vector<Balloon> balloons;
    std::vector<PanelBody> bodies;
};

// The single-balloon layout request LayoutBalloon fills (panel.cpp:928): text
// already broken into lines (Item 2.4), the font metrics, the speaker anchor,
// and the placement rect chosen by GetCloudEstimate/GetInterveningBBox.
struct BalloonRequest final {
    BalloonShapeKind kind{};
    std::string text;
    std::vector<TextLine> lines;
    FontMetrics font;
    int arrow_x{};       // speaker m_arrowX, panel twips
    int speaker_top{};   // speaker m_bbox.Top, panel twips (<= 0)
    int place_left{};    // brect.left from GetCloudEstimate
    int place_top{};     // brect.top (freeRect.top, usually 0)
};

// LayoutBalloon core (panel.cpp:928) for one balloon: build the cloud (or box),
// position it via SetBBox + DockAtTop, compute the route region and tail. Pure
// geometry (the intervening-bbox multi-balloon shuffle is left to the caller /
// get_intervening_top below); deterministic and Linux-testable.
[[nodiscard]] auto layout_balloon(const BalloonRequest& request) -> Balloon;

} // namespace comicchat
