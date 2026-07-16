#pragma once

#include "comicchat/cpp26.hpp"
#include "comicchat/net/connection_engine.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <stop_token>

namespace comicchat {

#if !defined(COMICCHAT_DETERMINISTIC_DEFAULT)
#define COMICCHAT_DETERMINISTIC_DEFAULT 0
#endif

enum class SchedulerError { stopped, queue_full, stale_generation };

class WorkerScheduler final {
public:
    using Task = std::function<void(std::stop_token)>;

    explicit WorkerScheduler(std::size_t workers = 0, std::size_t capacity = 256,
                             bool deterministic = COMICCHAT_DETERMINISTIC_DEFAULT != 0);
    ~WorkerScheduler();
    WorkerScheduler(const WorkerScheduler&) = delete;
    auto operator=(const WorkerScheduler&) -> WorkerScheduler& = delete;

    [[nodiscard]] auto submit(net::GenerationId generation, Task task)
        -> std::expected<std::future<void>, SchedulerError>;
    void advance_generation(net::GenerationId generation);
    void stop() noexcept;
    [[nodiscard]] auto worker_count() const noexcept -> std::size_t;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace comicchat
