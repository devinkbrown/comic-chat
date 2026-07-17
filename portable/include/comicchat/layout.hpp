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

} // namespace comicchat
