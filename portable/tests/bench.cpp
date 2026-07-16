#include "comicchat/layout.hpp"
#include "comicchat/memory.hpp"
#include "comicchat/scheduler.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>

namespace {

template <class Callable>
auto measure(const std::size_t iterations, Callable&& callable) -> std::chrono::nanoseconds {
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < iterations; ++index) callable(index);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
}

void report(const char* name, const std::chrono::nanoseconds elapsed, const std::size_t iterations) {
    std::cout << name << ' ' << elapsed.count() / static_cast<std::int64_t>(iterations) << " ns/op\n";
}

} // namespace

auto main() -> int {
    std::int64_t checksum{};
    constexpr std::size_t layout_iterations = 250'000;
    const auto layout_time = measure(layout_iterations, [&](const auto index) {
        const auto rect = comicchat::panel_rect(index % 100, 0, 0);
        checksum += rect.left + rect.bottom;
    });
    report("panel-layout", layout_time, layout_iterations);

    comicchat::FrameArena arena{256U * 1024U};
    constexpr std::size_t batch_iterations = 5'000;
    const auto batch_time = measure(batch_iterations, [&](const auto generation) {
        {
            comicchat::RenderBatchBuilder builder{arena, 512};
            for (std::size_t item = 0; item < 256; ++item) {
                if (!builder.push({comicchat::PrimitiveKind::solid, 0, 0, 0, 0, 10, 10,
                                   static_cast<std::uint32_t>(item), 0})) return;
            }
            const auto snapshot = builder.finalize(generation);
            if (snapshot) checksum += static_cast<std::int64_t>((*snapshot)->primitives().size());
        }
        arena.reset();
    });
    report("pmr-render-batch-256", batch_time, batch_iterations);

    comicchat::WorkerScheduler scheduler{1, 64, true};
    scheduler.advance_generation(1);
    constexpr std::size_t task_iterations = 25'000;
    const auto scheduler_time = measure(task_iterations, [&](const auto) {
        auto future = scheduler.submit(1, [&](std::stop_token) { ++checksum; });
        if (future) future->get();
    });
    scheduler.stop();
    report("deterministic-task", scheduler_time, task_iterations);
    std::cout << "checksum " << checksum << '\n';
    return checksum == 0 ? 1 : 0;
}
