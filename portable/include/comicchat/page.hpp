#pragma once

#include "comicchat/balloon.hpp"
#include "comicchat/cpp26.hpp"
#include "comicchat/layout.hpp"
#include "comicchat/text.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Phase 2.5 — the PAGE COMPOSITION layer (render-port-spec.md §2.5): a faithful,
// line-for-line port of Microsoft's CUnitPanelPage::AddLine (panel.cpp:1061) and
// the CUnitPanel::LayoutAvatars / LayoutBalloons expert placement it drives
// (panel.cpp:728 / :858). It supersedes comic_page.cpp's single-body,
// single-balloon centered_left() simplification with the real multi-body /
// multi-balloon panel accumulation:
//
//   * AddLine clones the tail panel (or opens a fresh one), appends the speaker's
//     balloon, runs LayoutAvatars then LayoutBalloons, and on a multi-balloon
//     overflow discards the clone, StartNewPanel()s, and recurses — exactly the
//     panel.cpp:1082-1139 control flow.
//   * LayoutAvatars distributes the placed bodies with order_conversation()
//     (the ported OrderAvatars/DoGreedyOrdering/EvalPair, layout.hpp) and the
//     LayoutAvatars margin formula (panel.cpp:810), deriving each speaker's
//     m_arrowX via arrow_anchors().
//   * LayoutBalloons seeds one MsvcrtRandom per panel (srand(m_seed),
//     panel.cpp:870) and lays each balloon in element order with cloud_estimate()
//     + the ported GetInterveningBBox() route-region routing (panel.cpp:167),
//     threading the panel PRNG through every balloon in the exact Microsoft
//     rand() order: GetCloudEstimate draws THEN the ShiftLines per-line draws.
//
// HONEST LIMITS (see page.cpp for the anchored detail):
//   * Body dimensions (width/height/face_fraction) are consumed pre-fitted from
//     the caller's avatar-selection; the GetDimInfo art-scaling/zoom of
//     LayoutAvatars (panel.cpp:759-808) is Item 2.2 and is NOT re-derived here.
//   * The portable renderer pre-wraps text to a fixed width; the source re-wraps
//     to the chosen goalWidth inside SetBBox, so the ShiftLines draw COUNT tracks
//     the portable line count, not the source goalWidth re-wrap.
//   * RearrangeBalloons is a shipped no-op (panel.cpp:962) and is matched as one.
//   * SplitHeight overflow of a single too-tall balloon is approximated at line
//     granularity (see force_fit_split in page.cpp).

namespace comicchat {

// CUnitPanel::m_borderWidth (panel.cpp:64).
inline constexpr std::int32_t panel_border_width = 60;

// Pre-fitted body / wrap defaults for the single-speaker slot (kept in sync with
// comic_page.hpp's message_* values but declared here so the page module does not
// depend on that unit being edited by the integration agent).
inline constexpr std::int32_t page_default_body_width = 800;
inline constexpr std::int32_t page_default_body_height = 840;
inline constexpr std::int32_t page_default_max_text_width = 1200;

// One avatar's selection: identity, pre-fitted body geometry (Item 2.2 output),
// tint, and its "talk-to" partners (CUserInfo::m_udi.m_talkTos) consulted by the
// LayoutAvatars ordering. Registered page-wide as lines arrive, exactly as the
// global CAvatarX registry the source consults. (Named PageAvatar to avoid the
// avatar_assets.hpp AvatarSelection asset-picking type.)
struct PageAvatar final {
    std::uint32_t avatar_id{};
    std::int32_t body_width{page_default_body_width};    // fitted body width, panel twips
    std::int32_t body_height{page_default_body_height};  // fitted body height, panel twips
    double face_fraction{0.5};  // faceX / width at the unflipped (right) pose
    std::uint32_t color{0x6c8ebfU};
    std::vector<std::uint32_t> talk_tos{};
    auto operator==(const PageAvatar&) const -> bool = default;
};

// One chat line fed to AddLine: who speaks, what they say, and the balloon mode
// bits (bm_say / bm_whisper / bm_think / bm_action, balloon.hpp).
struct Line final {
    PageAvatar speaker;
    std::string text;
    std::uint16_t modes{bm_say};
};

// Page geometry + the say-font the balloons lay out with. Defaults mirror the
// portable panel model (2300-twip square, 60-twip border). `seed` feeds the
// page-level rand() that draws each panel's m_seed (panel.cpp:558).
struct PageConfig final {
    std::int32_t unit_width{logical_panel_width};
    std::int32_t unit_height{logical_panel_height};
    std::int32_t border_width{panel_border_width};
    std::int32_t max_text_width{page_default_max_text_width};
    FontMetrics font{};
    std::uint32_t seed{1U};
};

// Per-panel provenance for tests / callers: the panel's m_seed, the element
// (speak) order of its balloons, and the left-to-right body placement order.
struct PanelInfo final {
    std::uint32_t seed{};
    std::vector<std::uint32_t> speak_order;  // balloon/element order (rand() order)
    std::vector<std::uint32_t> body_order;   // arrow_anchors left-to-right order
};

// The page-composition driver. Feed it lines; read back the assembled sequence of
// balloon.hpp Panels (bodies + balloons in panel-local twips, Y-up) and the
// matching PanelInfo provenance.
class Page final {
public:
    // `measure` wraps balloon text (break_into_lines); inject a synthetic
    // (monospace) measure for TTF-free tests or measure_text_width for the live
    // path. `config.font` supplies the balloon line metrics.
    Page(PageConfig config, TextMeasure measure);

    // Register a channel participant's avatar selection up front (the GetAvatar
    // registry the source consults). Needed for talk-to partners that may be
    // pulled into a panel by LayoutAvatars before they ever speak; a speaker is
    // also auto-registered from its Line on add_line.
    void add_participant(const PageAvatar& selection);

    // Port of CUnitPanelPage::AddLine (panel.cpp:1061). Accumulates the line into
    // the tail panel (cloning it) or opens a new panel, lays avatars + balloons,
    // splits on overflow, and recurses on the leftover — all deterministic under
    // config.seed.
    void add_line(const Line& line);

    [[nodiscard]] auto panels() const noexcept -> const std::vector<Panel>&;
    [[nodiscard]] auto panel_infos() const noexcept -> const std::vector<PanelInfo>&;

private:
    struct PanelState final {
        Panel rendered;
        PanelInfo info;
        std::uint32_t seed{};
        std::vector<Line> elements;      // one balloon per distinct speaker, speak order
        HistoresisMap historesis_in;     // placement memory before this panel
        HistoresisMap historesis_out;    // ... after (feeds the next panel)
    };

    enum class Fit { fit, fit_with_leftover, overflow };
    struct LayoutOutcome final {
        Fit status{Fit::fit};
        std::string leftover;
    };

    void register_avatar(const PageAvatar& selection);
    [[nodiscard]] auto layout_panel(PanelState& draft) -> LayoutOutcome;

    PageConfig config_;
    TextMeasure measure_;
    MsvcrtRandom seed_rng_;            // the page-level rand() feeding m_seed
    bool new_panel_pending_{true};     // CPage() m_newPanel = TRUE (panel.h:78)
    std::map<std::uint32_t, PageAvatar> registry_;  // CAvatarX registry
    std::vector<PanelState> states_;
    std::vector<Panel> panels_;
    std::vector<PanelInfo> infos_;
};

} // namespace comicchat
