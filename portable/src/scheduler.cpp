#include "comicchat/scheduler.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
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
        threading::StopToken generation_token;
    };

    Impl(std::size_t requested, const std::size_t capacity, const bool deterministic)
        : capacity_{capacity}, deterministic_{deterministic} {
        if (requested == 0) {
            requested = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8);
        }
        if (!deterministic_) {
            workers_.reserve(requested);
            for (std::size_t index = 0; index < requested; ++index) {
                workers_.emplace_back([this](const threading::StopToken token) { run(token); });
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
        Queued queued{generation, std::move(task), {}, generation_stop_.get_token()};
        auto future = queued.promise.get_future();
        if (deterministic_) {
            lock.unlock();
            std::exception_ptr failure;
            try {
                queued.task(queued.generation_token);
            } catch (...) {
                failure = std::current_exception();
            }
            complete_if_current(queued, failure);
        } else {
            queue_.push_back(std::move(queued));
            lock.unlock();
            ready_.notify_one();
        }
        return future;
    }

    void advance(const net::GenerationId generation) {
        std::scoped_lock lock{mutex_};
        (void)generation_stop_.request_stop();
        generation_ = generation;
        generation_stop_ = threading::StopSource{};
        // Destroying an unfulfilled promise completes its future with the
        // allocation-free broken_promise state.
        queue_.clear();
    }

    void stop() noexcept {
        {
            std::scoped_lock lock{mutex_};
            stopped_ = true;
            (void)generation_stop_.request_stop();
            queue_.clear();
        }
        for (auto& worker : workers_) worker.request_stop();
        ready_.notify_all();
        if (std::ranges::any_of(workers_, [](const threading::JThread& worker) {
                return worker.get_id() == std::this_thread::get_id();
            })) return;
        bool joined = true;
        for (auto& worker : workers_) {
            if (!worker.joinable()) continue;
            try {
                worker.join();
            } catch (...) {
                joined = false;
            }
        }
        if (joined) workers_.clear();
    }

    auto count() const noexcept -> std::size_t { return deterministic_ ? 1 : workers_.size(); }

private:
    void run(const threading::StopToken token) {
        while (!token.stop_requested()) {
            std::unique_lock lock{mutex_};
            ready_.wait(lock, [&] { return stopped_ || token.stop_requested() || !queue_.empty(); });
            if (stopped_ || token.stop_requested()) return;
            auto queued = std::move(queue_.front());
            queue_.pop_front();
            lock.unlock();
            std::exception_ptr failure;
            try {
                queued.task(queued.generation_token);
            } catch (...) {
                failure = std::current_exception();
            }
            complete_if_current(queued, failure);
        }
    }

    void complete_if_current(Queued& queued, const std::exception_ptr& failure) noexcept {
        std::scoped_lock lock{mutex_};
        if (stopped_ || queued.generation != generation_ || queued.generation_token.stop_requested()) return;
        try {
            if (failure) queued.promise.set_exception(failure);
            else queued.promise.set_value();
        } catch (...) {
        }
    }

    std::size_t capacity_{};
    bool deterministic_{};
    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<Queued> queue_;
    std::vector<threading::JThread> workers_;
    threading::StopSource generation_stop_;
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
