#pragma once

#include "comicchat/cpp26.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
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

// Deterministic reproduction of the Microsoft C runtime (MSVCRT) rand()/srand().
//
// Comic balloon layout is randomized-but-deterministic per panel: each CPanel
// draws m_seed = rand() at construction (panel.cpp:558; a cloned panel copies it,
// panel.cpp:565) and calls srand(m_seed) before laying any balloon out
// (panel.cpp:870, "always layout panel the same random way"). Every stochastic
// choice downstream consumes randfloat() = rand()/RAND_MAX (balloon.cpp:444,446).
//
// To reproduce a captured m_seed byte-for-byte the portable renderer must
// reimplement MSVCRT's exact 32-bit LCG rather than std::mt19937 or the host
// libc rand(): state = state*214013 + 2531011; return (state >> 16) & 0x7fff,
// with RAND_MAX = 0x7fff and a default seed of 1. This is the only PRNG whose
// sequence matches seeds stored in real Comic Chat .ccc files, which is what
// makes re-layout goldens reproducible on any host.
class MsvcrtRandom final {
public:
    // MSVCRT's RAND_MAX. rand() returns a value in [0, rand_max].
    static constexpr std::uint32_t rand_max = 0x7fffU;

    constexpr MsvcrtRandom() noexcept = default;
    constexpr explicit MsvcrtRandom(std::uint32_t seed_value) noexcept : state_{seed_value} {}

    // Equivalent to srand(seed_value): install the generator state.
    constexpr void seed(std::uint32_t seed_value) noexcept { state_ = seed_value; }

    // Equivalent to rand(): advance the LCG and return the next value in
    // [0, rand_max]. Bit-identical to MSVCRT for any given prior state.
    [[nodiscard]] constexpr auto next() noexcept -> std::uint32_t {
        state_ = state_ * 214013U + 2531011U;
        return (state_ >> 16U) & rand_max;
    }

    // Equivalent to randfloat() = ((double) rand()) / RAND_MAX (balloon.cpp:446),
    // yielding a value in [0.0, 1.0].
    [[nodiscard]] constexpr auto next_float() noexcept -> double {
        return static_cast<double>(next()) / static_cast<double>(rand_max);
    }

private:
    // MSVCRT's default rand() state when srand() is never called is 1.
    std::uint32_t state_{1U};
};

// A point in a panel's logical drawing space: Win32 MM_TWIPS
// (UNITSPERINCH = 1440), Y-up, with the panel interior running from top = 0 down
// to bottom = -m_unitHeight (panel.cpp:842, 772). Sign convention kept exactly as
// the source until the final device transform (render-port-spec.md §0).
struct LogicalPoint final {
    double x{};
    double y{};
    auto operator==(const LogicalPoint&) const -> bool = default;
};

// A point in device (Cairo) pixel space: origin top-left, Y-down.
struct DevicePoint final {
    double x{};
    double y{};
    auto operator==(const DevicePoint&) const -> bool = default;
};

// The single device transform every logical drawing pass applies, exactly the
// composition render_title_panel uses (render.cpp): translate(origin) then
// scale(scale, -scale). It maps twips/Y-up logical coordinates to device pixels.
struct PanelTransform final {
    double origin_x{};  // device pixel of logical (0, 0) — the panel's top-left.
    double origin_y{};
    double scale{1.0};  // device pixels per logical twip.

    [[nodiscard]] constexpr auto to_device(const LogicalPoint point) const noexcept -> DevicePoint {
        return {origin_x + point.x * scale, origin_y - point.y * scale};
    }
};

// Fit a square logical panel of `source_units` twips into a device canvas,
// centered and aspect-preserving, mirroring render_title_panel's panel_size /
// scale / origin computation (render.cpp).
[[nodiscard]] auto fit_panel_transform(std::int32_t canvas_width, std::int32_t canvas_height,
                                       double source_units) -> PanelTransform;

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

// -------------------------------------------------------------------------
// Item 2.3b — conversation-panel expert placement (render-port-spec.md §2.3.b).
//
// This ports LayoutAvatars' greedy left-to-right ordering of the speakers in a
// conversation panel: OrderAvatars -> AddTalkTos -> DoGreedyOrdering ->
// EvalPlacement -> EvalPair + ComputeDisplacementPenalty (panel.cpp:280-450),
// plus UpdateHistoresis (panel.cpp:437). It is pure integer/ordering logic over
// avatar identities and the "who talks to whom" graph — no rasterization and no
// PRNG. The source ordering path never calls rand(): ties in EvalPlacement break
// on the avatar's remembered facing (m_lastDir) and DoGreedyOrdering keeps the
// first best position via a strict `<`, so the result is deterministic by
// construction. (rand()/MsvcrtRandom is consumed only by 2.1 balloon layout.)
//
// Facing convention (m_flip): false = facing RIGHT (toward a higher index,
// the source default FALSE), true = facing LEFT. To address someone on your
// right you face right (flip == false); EvalPair rewards facing the people you
// talk to and heavily penalizes facing away (panel.cpp:296-309).

// One avatar known to the placement pass, with the avatars it addresses
// ("talk-tos", CUserInfo::m_udi.m_talkTos). The registry of these is both the
// talk-to graph consulted by EvalPair and the existence check AddTalkTos uses
// when it pulls a conversation partner into the panel (panel.cpp:336-343).
struct ConversationAvatar final {
    std::uint32_t avatar_id{};
    std::vector<std::uint32_t> talk_tos{};  // avatar_ids this speaker is addressing
    auto operator==(const ConversationAvatar&) const -> bool = default;
};

// Cross-panel placement memory for one avatar (CAvatarX::m_lastDir/m_lastLeft/
// m_lastRight, panel.cpp:442-448). Fresh avatars use the source defaults
// (avatar.cpp:896-898): facing right, no remembered neighbours (id 0).
struct AvatarHistoresis final {
    bool last_dir{false};        // remembered facing, breaks EvalPlacement ties
    std::uint32_t last_left{0};  // avatar_id last seen to this one's left
    std::uint32_t last_right{0}; // avatar_id last seen to this one's right
    auto operator==(const AvatarHistoresis&) const -> bool = default;
};

// One placed body in the final left-to-right ordering.
struct PlacedBody final {
    std::uint32_t avatar_id{};
    bool requested{true};  // false = pulled in as a talk-to partner (BR_GOODIDEA)
    bool flip{};           // chosen facing: false = right, true = left
    auto operator==(const PlacedBody&) const -> bool = default;
};

using HistoresisMap = std::map<std::uint32_t, AvatarHistoresis>;

// Result of placing one conversation panel: the ordered bodies (each with the
// facing the greedy pass chose) plus the historesis updated for the next panel.
// `bodies` (avatar_id order + flip) is exactly what LayoutAvatars consumes to
// distribute horizontal margins and derive each balloon's tail anchor m_arrowX
// (panel.cpp:810,817); see arrow_anchors() for the arrowX projection Item 2.1
// reads.
struct ConversationOrder final {
    std::vector<PlacedBody> bodies;
    HistoresisMap historesis;
    auto operator==(const ConversationOrder&) const -> bool = default;
};

// Greedy conversation-panel placement (OrderAvatars, panel.cpp:426):
//   * `speaker_ids` are the avatars that actually spoke, in panel/element order;
//   * `avatars` is the registry of every avatar the pass may consult — talk-to
//     graph for EvalPair and existence check for AddTalkTos;
//   * `historesis` is the remembered state from prior panels (missing avatars
//     take the source defaults).
// With fewer than five speakers AddTalkTos first pulls in addressed partners
// (capped at five per panel, panel.cpp:325), then DoGreedyOrdering inserts each
// body at the position + facing that minimises the rating. The returned
// `historesis` folds UpdateHistoresis over the result for the next panel.
[[nodiscard]] auto order_conversation(const std::vector<std::uint32_t>& speaker_ids,
                                      const std::vector<ConversationAvatar>& avatars,
                                      const HistoresisMap& historesis) -> ConversationOrder;

// State the AddLine flow-control split predicate reads (panel.cpp:1082): the
// tail panel's element count, how many panels exist, and the m_newPanel flag
// (StartNewPanel, panel.h:91; set at page start so the first line always opens a
// panel, panel.h:78).
struct PanelSplitState final {
    std::size_t tail_panel_elements{};  // pOldP->m_elements.GetCount()
    std::size_t panel_count{};          // m_panels.GetCount()
    bool new_panel_pending{};           // m_newPanel (a prior StartNewPanel)
};

// The AddLine panel-split predicate (panel.cpp:1067,1082): a fresh CUnitPanel is
// started when an action box forces it, when a StartNewPanel is pending, when the
// tail panel already holds five elements, when fewer than two panels exist, or
// when the speaker is already present in the tail panel (AvatarInPanel).
[[nodiscard]] auto should_start_new_panel(const PanelSplitState& state,
                                          bool speaker_already_in_panel,
                                          bool is_action_box) -> bool;

// The tail anchor projection Item 2.1 (balloon geometry) consumes: given the
// placed bodies' left-to-right order and each body's fitted width in panel twips,
// return each body's m_arrowX — the x where its balloon tail points at it.
// LayoutAvatars lays bodies edge-to-edge across the drawable interior, then sets
// m_arrowX = box.Left + round(face_fraction * width) with face_fraction flipped
// under m_flip (panel.cpp:810,817; avatar.cpp:73-74,112-113). Placement owns the
// order + facing; the caller supplies real fitted widths and per-avatar
// face fractions once Item 2.2 dimension info exists.
struct BodySlot final {
    std::uint32_t avatar_id{};
    std::int32_t width{};        // fitted body width, panel twips
    double face_fraction{};      // faceX / width in [0,1] at the unflipped pose
    bool flip{};                 // facing chosen by placement
};

// x of each body's left edge and its tail anchor, laid left-to-right from
// `interior_left`. Bodies are packed with `gap` twips between them (LayoutAvatars
// distributes the slack as margins; a caller that has computed the real margin
// passes it as `gap`). face_fraction is mirrored to (1 - face_fraction) when the
// body is flipped, exactly as GetDimInfo flips faceX under m_flip.
struct ArrowAnchor final {
    std::uint32_t avatar_id{};
    std::int32_t left{};     // body box left edge, panel twips
    std::int32_t arrow_x{};  // m_arrowX tail anchor, panel twips
    auto operator==(const ArrowAnchor&) const -> bool = default;
};

[[nodiscard]] auto arrow_anchors(const std::vector<BodySlot>& slots, std::int32_t interior_left,
                                 std::int32_t gap) -> std::vector<ArrowAnchor>;

} // namespace comicchat
