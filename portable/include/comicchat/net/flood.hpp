#pragma once

#include "comicchat/cpp26.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>

namespace comicchat::net {

enum class FloodThreshold {
    at_limit,
    over_limit,
};

// Fixed-storage monotonic window used by both legacy user flood protection and
// automation-loop protection. It cannot wrap with wall-clock/DST changes.
class MonotonicFloodWindow final {
public:
    using clock = std::chrono::steady_clock;

    [[nodiscard]] auto record(
        std::size_t limit,
        std::chrono::milliseconds interval,
        FloodThreshold threshold = FloodThreshold::over_limit) noexcept -> bool {
        return record_at(clock::now(), limit, interval, threshold);
    }

    [[nodiscard]] auto record_at(
        clock::time_point now,
        std::size_t limit,
        std::chrono::milliseconds interval,
        FloodThreshold threshold = FloodThreshold::over_limit) noexcept -> bool {
        if (!started_ || interval <= std::chrono::milliseconds::zero() ||
            now < window_start_ || now - window_start_ > interval) {
            window_start_ = now;
            occurrences_ = 1;
            started_ = true;
        } else if (occurrences_ != (std::numeric_limits<std::size_t>::max)()) {
            ++occurrences_;
        }
        return threshold == FloodThreshold::at_limit
            ? occurrences_ >= (std::max<std::size_t>)(2, limit)
            : occurrences_ > limit;
    }

    void reset() noexcept {
        started_ = false;
        occurrences_ = 0;
    }

    [[nodiscard]] auto occurrences() const noexcept -> std::size_t { return occurrences_; }

private:
    clock::time_point window_start_{};
    std::size_t occurrences_{};
    bool started_{};
};

} // namespace comicchat::net
