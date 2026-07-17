#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <version>

namespace comicchat::threading {

#if !defined(COMICCHAT_FORCE_THREAD_FALLBACK) && \
    defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L

using StopToken = std::stop_token;
using StopSource = std::stop_source;
using JThread = std::jthread;

#else

namespace detail {
struct StopState final {
    std::atomic_bool requested{};
};
} // namespace detail

class StopToken final {
public:
    StopToken() = default;

    [[nodiscard]] auto stop_requested() const noexcept -> bool {
        return state_ && state_->requested.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto stop_possible() const noexcept -> bool { return state_ != nullptr; }

private:
    explicit StopToken(std::shared_ptr<detail::StopState> state) noexcept : state_{std::move(state)} {}

    std::shared_ptr<detail::StopState> state_;
    friend class StopSource;
};

class StopSource final {
public:
    StopSource() : state_{std::make_shared<detail::StopState>()} {}

    [[nodiscard]] auto get_token() const noexcept -> StopToken { return StopToken{state_}; }

    auto request_stop() noexcept -> bool {
        return state_ && !state_->requested.exchange(true, std::memory_order_acq_rel);
    }

private:
    std::shared_ptr<detail::StopState> state_;
};

// libc++ on FreeBSD 15 does not yet provide C++20 jthread. This fallback owns
// its std::thread for its entire lifetime: it never detaches, and a destructor
// reached by its own worker terminates rather than creating a use-after-free.
class JThread final {
public:
    JThread() = default;

    template <typename Function>
    explicit JThread(Function&& function) : stop_{}, thread_{start(std::forward<Function>(function), stop_.get_token())} {}

    ~JThread() { stop_and_join(); }

    JThread(const JThread&) = delete;
    auto operator=(const JThread&) -> JThread& = delete;

    JThread(JThread&& other) noexcept
        : stop_{std::move(other.stop_)}, thread_{std::move(other.thread_)} {}

    auto operator=(JThread&& other) noexcept -> JThread& {
        if (this == &other) return *this;
        stop_and_join();
        stop_ = std::move(other.stop_);
        thread_ = std::move(other.thread_);
        return *this;
    }

    [[nodiscard]] auto joinable() const noexcept -> bool { return thread_.joinable(); }
    [[nodiscard]] auto get_id() const noexcept -> std::thread::id { return thread_.get_id(); }
    [[nodiscard]] auto get_stop_token() const noexcept -> StopToken { return stop_.get_token(); }
    auto request_stop() noexcept -> bool { return stop_.request_stop(); }
    void join() { thread_.join(); }

private:
    template <typename Function>
    static auto start(Function&& function, StopToken token) -> std::thread {
        using Callable = std::decay_t<Function>;
        return std::thread{[callable = Callable(std::forward<Function>(function)), token = std::move(token)]() mutable {
            if constexpr (std::is_invocable_v<Callable, StopToken>)
                std::invoke(std::move(callable), token);
            else
                std::invoke(std::move(callable));
        }};
    }

    void stop_and_join() noexcept {
        (void)request_stop();
        if (!thread_.joinable()) return;
        if (thread_.get_id() == std::this_thread::get_id()) std::terminate();
        try {
            thread_.join();
        } catch (...) {
            std::terminate();
        }
    }

    StopSource stop_;
    std::thread thread_;
};

#endif

} // namespace comicchat::threading
