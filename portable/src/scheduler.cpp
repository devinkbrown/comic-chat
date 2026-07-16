#include "comicchat/scheduler.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace comicchat {

class WorkerScheduler::Impl final {
public:
    struct Queued final {
        net::GenerationId generation{};
        Task task;
        std::promise<void> promise;
    };

    Impl(std::size_t requested, const std::size_t capacity, const bool deterministic)
        : capacity_{capacity}, deterministic_{deterministic} {
        if (requested == 0) {
            requested = std::max(1U, std::thread::hardware_concurrency());
        }
        if (!deterministic_) {
            workers_.reserve(requested);
            for (std::size_t index = 0; index < requested; ++index) {
                workers_.emplace_back([this](const std::stop_token token) { run(token); });
            }
        }
    }

    ~Impl() { stop(); }

    auto submit(const net::GenerationId generation, Task task)
        -> std::expected<std::future<void>, SchedulerError> {
        std::unique_lock lock{mutex_};
        if (stopped_) return std::unexpected{SchedulerError::stopped};
        if (generation != generation_) return std::unexpected{SchedulerError::stale_generation};
        if (!deterministic_ && queue_.size() >= capacity_) return std::unexpected{SchedulerError::queue_full};
        Queued queued{generation, std::move(task), {}};
        auto future = queued.promise.get_future();
        if (deterministic_) {
            lock.unlock();
            queued.task({});
            queued.promise.set_value();
        } else {
            queue_.push_back(std::move(queued));
            lock.unlock();
            ready_.notify_one();
        }
        return future;
    }

    void advance(const net::GenerationId generation) {
        std::scoped_lock lock{mutex_};
        generation_ = generation;
        for (auto& queued : queue_) queued.promise.set_exception(
            std::make_exception_ptr(std::runtime_error{"task generation cancelled"}));
        queue_.clear();
    }

    void stop() noexcept {
        {
            std::scoped_lock lock{mutex_};
            if (stopped_) return;
            stopped_ = true;
            for (auto& queued : queue_) queued.promise.set_exception(
                std::make_exception_ptr(std::runtime_error{"scheduler stopped"}));
            queue_.clear();
        }
        for (auto& worker : workers_) worker.request_stop();
        ready_.notify_all();
        workers_.clear();
    }

    auto count() const noexcept -> std::size_t { return deterministic_ ? 1 : workers_.size(); }

private:
    void run(const std::stop_token token) {
        while (!token.stop_requested()) {
            std::unique_lock lock{mutex_};
            ready_.wait(lock, token, [this] { return stopped_ || !queue_.empty(); });
            if (stopped_ || token.stop_requested()) return;
            auto queued = std::move(queue_.front());
            queue_.pop_front();
            const auto current = generation_;
            lock.unlock();
            if (queued.generation != current) {
                queued.promise.set_exception(std::make_exception_ptr(
                    std::runtime_error{"task generation cancelled"}));
                continue;
            }
            try {
                queued.task(token);
                queued.promise.set_value();
            } catch (...) {
                queued.promise.set_exception(std::current_exception());
            }
        }
    }

    std::size_t capacity_{};
    bool deterministic_{};
    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<Queued> queue_;
    std::vector<std::jthread> workers_;
    net::GenerationId generation_{};
    bool stopped_{};
};

WorkerScheduler::WorkerScheduler(const std::size_t workers, const std::size_t capacity, const bool deterministic)
    : impl_{std::make_unique<Impl>(workers, capacity, deterministic)} {}
WorkerScheduler::~WorkerScheduler() = default;
auto WorkerScheduler::submit(const net::GenerationId generation, Task task)
    -> std::expected<std::future<void>, SchedulerError> { return impl_->submit(generation, std::move(task)); }
void WorkerScheduler::advance_generation(const net::GenerationId generation) { impl_->advance(generation); }
void WorkerScheduler::stop() noexcept { impl_->stop(); }
auto WorkerScheduler::worker_count() const noexcept -> std::size_t { return impl_->count(); }

} // namespace comicchat
