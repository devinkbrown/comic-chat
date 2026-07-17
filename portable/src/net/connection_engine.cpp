#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "comicchat/net/connection_engine.hpp"
#include "comicchat/crypto_runtime.hpp"
#include "comicchat/memory.hpp"
#include "comicchat/thread_compat.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#include <uv.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#else
#include <wincrypt.h>
#endif

static_assert(MBEDTLS_VERSION_NUMBER == 0x03060700, "Comic Chat requires the pinned mbedTLS 3.6.7 ABI");
#if !defined(MBEDTLS_AES_ROM_TABLES) || defined(MBEDTLS_SELF_TEST)
#error "Comic Chat requires immutable AES tables and a production mbedTLS configuration"
#endif

namespace comicchat::net {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::chrono::milliseconds happy_eyeballs_delay{250};
constexpr std::size_t io_chunk = 16U * 1024U;
constexpr std::size_t proxy_reply_limit = 16U * 1024U;
constexpr std::size_t event_control_headroom = 2;
constexpr std::array<std::size_t, 5> scheduling_order{2, 1, 0, 3, 4};
constexpr std::array<unsigned int, 5> scheduling_quanta{4, 4, 4, 2, 1};

auto command_generation(const Command& command) -> GenerationId {
    return std::visit([](const auto& value) { return value.generation; }, command);
}

auto priority_index(const Command& command) -> std::size_t {
    const auto* send = std::get_if<Send>(&command);
    return send ? static_cast<std::size_t>(send->priority) : static_cast<std::size_t>(Priority::control);
}

auto mix_seed(std::uint64_t value) noexcept -> std::uint64_t {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

auto entropy_seed() noexcept -> std::uint64_t {
    std::uint64_t seed{};
    if (uv_random(nullptr, nullptr, &seed, sizeof(seed), 0, nullptr) == 0) return seed;

    // uv_random is expected to succeed on supported Windows and Unix hosts.
    // Preserve per-engine diversity on an unusually constrained host.
    static std::atomic_uint64_t sequence{};
    const auto clock = static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
    return mix_seed(clock ^ sequence.fetch_add(1, std::memory_order_relaxed));
}

auto retry_delay(const unsigned int failures, const std::uint64_t jitter_seed) -> std::chrono::milliseconds {
    const auto exponent = std::min(failures, 6U);
    const auto base = std::chrono::milliseconds{500U * (1U << exponent)};
    const auto jitter = std::chrono::milliseconds{mix_seed(jitter_seed + failures) % 251U};
    return std::min(base + jitter, std::chrono::milliseconds{60'000});
}

void secure_clear(std::string& value) noexcept {
    if (!value.empty()) mbedtls_platform_zeroize(value.data(), value.size());
    value.clear();
}

auto safe_network_name(const std::string_view value) noexcept -> bool {
    if (value.empty() || value.size() > 253) return false;
    return std::ranges::none_of(value, [](const unsigned char byte) {
        return byte <= 0x20U || byte == 0x7fU;
    });
}

auto safe_endpoint_host(const std::string_view value) noexcept -> bool {
    if (!safe_network_name(value) || value.contains('[') || value.contains(']')) return false;
    if (!value.contains(':')) return true;
    sockaddr_in6 address{};
    return uv_inet_pton(AF_INET6, value.data(), &address.sin6_addr) == 0;
}

auto http_authority(const Endpoint& endpoint) -> std::string {
    const auto port = std::to_string(endpoint.port);
    if (endpoint.host.contains(':')) return '[' + endpoint.host + "]:" + port;
    return endpoint.host + ':' + port;
}

auto safe_path(const std::string_view value) noexcept -> bool {
    return !value.empty() && std::ranges::none_of(value, [](const unsigned char byte) {
        return byte < 0x20U || byte == 0x7fU;
    });
}

} // namespace

class ConnectionEngine::Impl final {
public:
    Impl() {
        crypto_ready_ = crypto::initialize_runtime();
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&tls_config_);
        mbedtls_x509_crt_init(&ca_);
        mbedtls_ctr_drbg_init(&random_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ssl_session_init(&session_);
    }

    ~Impl() {
        stop();
        mbedtls_ssl_session_free(&session_);
        mbedtls_entropy_free(&entropy_);
        mbedtls_ctr_drbg_free(&random_);
        mbedtls_x509_crt_free(&ca_);
        mbedtls_ssl_config_free(&tls_config_);
        mbedtls_ssl_free(&ssl_);
    }

    auto start(ConnectionOptions options) -> std::expected<GenerationId, EngineError> {
        bool already_running{};
        {
            std::scoped_lock lock{mutex_};
            // Assigning a jthread over its own joinable worker terminates (or
            // throws into a noexcept move assignment). A stopped worker may be
            // joined and reused by an external caller, but never by itself.
            already_running = !stopped_ ||
                (thread_.joinable() && thread_.get_id() == std::this_thread::get_id());
        }
        if (already_running) {
            if (options.proxy.password) secure_clear(*options.proxy.password);
            return std::unexpected{EngineError::already_running};
        }
        if (!crypto_ready_) return std::unexpected{EngineError::crypto_unavailable};
        if (options.security == Security::tls && options.server_name.empty()) options.server_name = options.endpoint.host;
        if (!valid_options(options)) return std::unexpected{EngineError::invalid_options};
        std::optional<LockedSecret> proxy_password;
        if (options.proxy.password) {
            auto locked = LockedSecret::copy(*options.proxy.password);
            if (!locked) {
                secure_clear(*options.proxy.password);
                return std::unexpected{locked.error() == SecretError::lock_failed
                    ? EngineError::credential_lock_failed
                    : EngineError::invalid_options};
            }
            proxy_password.emplace(std::move(*locked));
            secure_clear(*options.proxy.password);
            options.proxy.password.reset();
        }
        stop();
        reset_tls_configuration();
        {
            std::scoped_lock lock{mutex_};
            ++generation_;
            options_ = std::move(options);
            reconnect_jitter_seed_ = options_.reconnect_jitter_seed.value_or(entropy_jitter_seed_);
            proxy_password_ = std::move(proxy_password);
            queued_bytes_ = 0;
            queued_receive_bytes_ = 0;
            reserved_send_completions_ = 0;
            reserved_receive_events_ = 0;
            events_.clear();
            peak_queued_events_.store(0, std::memory_order_relaxed);
            target_buckets_.clear();
            target_serial_ = 0;
            stopped_ = false;
        }
        thread_ = threading::JThread{[this](const threading::StopToken token) noexcept { network_thread_entry(token); }};
        const auto posted = post(Connect{generation(), options_});
        if (!posted) {
            stop();
            return std::unexpected{posted.error()};
        }
        return generation();
    }

    auto post(Command command) -> std::expected<void, EngineError> {
        std::scoped_lock lock{mutex_};
        const auto reject = [this, &command](const EngineError error) -> std::expected<void, EngineError> {
            if (auto* send = std::get_if<Send>(&command); send != nullptr && send->sensitive && !send->bytes.empty()) {
                const auto byte_count = send->bytes.size();
                mbedtls_platform_zeroize(send->bytes.data(), byte_count);
                send->bytes.clear();
                rejected_sensitive_bytes_wiped_.fetch_add(byte_count, std::memory_order_relaxed);
            }
            return std::unexpected{error};
        };
        if (stopped_) return reject(EngineError::not_running);
        if (command_generation(command) != generation_) return reject(EngineError::stale_generation);
        const auto* send = std::get_if<Send>(&command);
        const auto send_size = send == nullptr ? std::size_t{} : send->bytes.size();
        if (send != nullptr && (send->target.size() > 512 ||
            std::ranges::any_of(send->target, [](const unsigned char byte) {
                return byte < 0x20U || byte == 0x7fU;
            }))) return reject(EngineError::invalid_options);
        if (send != nullptr && (send_size == 0 || send_size > options_.limits.transmit_bytes ||
                               queued_bytes_ > options_.limits.transmit_bytes - send_size)) {
            return reject(EngineError::queue_full);
        }
        std::size_t queued{};
        for (const auto& queue : commands_) queued += queue.size();
        if (queued >= options_.limits.queued_commands) return reject(EngineError::queue_full);
        if (send != nullptr && event_occupancy_locked() >= ordinary_event_capacity_locked())
            return reject(EngineError::queue_full);
        commands_[priority_index(command)].push_back(std::move(command));
        queued_bytes_ += send_size;
        if (send != nullptr) ++reserved_send_completions_;
        if (loop_ready_.load(std::memory_order_acquire)) (void)uv_async_send(&command_wakeup_);
        return {};
    }

    auto poll(const std::size_t maximum) -> std::vector<Event> {
        std::vector<Event> result;
        bool resume{};
        {
            std::scoped_lock lock{mutex_};
            result.reserve(std::min(maximum, events_.size()));
            while (!events_.empty() && result.size() < maximum) {
                resume = true;
                if (const auto* received = std::get_if<BytesReceived>(&events_.front().body);
                    received != nullptr && received->bytes) {
                    const auto count = received->bytes->size();
                    queued_receive_bytes_ = count > queued_receive_bytes_ ? 0 : queued_receive_bytes_ - count;
                }
                if (events_.front().generation == generation_) result.push_back(std::move(events_.front()));
                events_.pop_front();
            }
            // Keep the readiness check and signal serialized with teardown.
            // libuv permits cross-thread sends, but not sends after uv_close.
            if (resume && loop_ready_.load(std::memory_order_acquire))
                (void)uv_async_send(&command_wakeup_);
        }
        return result;
    }

    void set_wakeup(std::function<void()> wakeup) {
        std::scoped_lock lock{mutex_};
        wakeup_ = std::move(wakeup);
    }

    void stop() noexcept {
        bool self_stop{};
        {
            std::scoped_lock lock{mutex_};
            stopped_ = true;
            if (!thread_.joinable()) return;
            thread_.request_stop();
            if (loop_ready_.load(std::memory_order_acquire)) (void)uv_async_send(&command_wakeup_);
            self_stop = thread_.get_id() == std::this_thread::get_id();
        }
        if (self_stop) return;
        try {
            thread_.join();
        } catch (...) {
            // A detached worker captures Impl and would become a destructor
            // use-after-free. The self-thread case is handled above; any other
            // join failure violates the engine lifetime contract.
            std::terminate();
        }
        std::scoped_lock lock{mutex_};
        clear_command_queues_locked();
        queued_bytes_ = 0;
        reserved_send_completions_ = 0;
        reserved_receive_events_ = 0;
        proxy_password_.reset();
    }

    auto generation() const noexcept -> GenerationId {
        std::scoped_lock lock{mutex_};
        return generation_;
    }

    auto stats() const noexcept -> EngineStats {
        std::scoped_lock lock{mutex_};
        std::size_t queued_commands{};
        for (const auto& queue : commands_) queued_commands += queue.size();
        return {
            loop_iterations_.load(std::memory_order_relaxed),
            command_wakeups_.load(std::memory_order_relaxed),
            rejected_sensitive_bytes_wiped_.load(std::memory_order_relaxed),
            peak_queued_events_.load(std::memory_order_relaxed),
            target_throttle_deferrals_.load(std::memory_order_relaxed),
            queued_bytes_,
            queued_commands,
        };
    }

private:
    enum class Pipeline { idle, resolving, connecting, proxy, tls, open, reconnect_wait };
    enum class ProxyPhase { none, socks_greeting, socks_auth, socks_connect, http_connect };

    struct Attempt final {
        uv_tcp_t tcp{};
        uv_connect_t request{};
        Impl* owner{};
    };

    struct ResolveRequest final {
        uv_getaddrinfo_t request{};
        Impl* owner{};
        std::uint64_t serial{};
        bool canceled{};
    };

    struct PendingWrite final {
        SendId id{};
        Priority priority{};
        std::vector<std::byte> bytes;
        std::size_t offset{};
        bool sensitive{};
        std::string target;
        bool target_token_charged{};
    };

    struct TokenBucket final {
        double tokens{64U * 1024U};
        Clock::time_point updated{Clock::now()};

        auto allowance(const std::size_t requested, const double bytes_per_second) -> std::size_t {
            const auto now = Clock::now();
            tokens = std::min<double>(64U * 1024U,
                tokens + std::chrono::duration<double>(now - updated).count() * bytes_per_second);
            updated = now;
            return std::min(requested, static_cast<std::size_t>(tokens));
        }

        void consume(const std::size_t bytes) noexcept { tokens = std::max(0.0, tokens - static_cast<double>(bytes)); }

        auto refill_delay(const double bytes_per_second) const noexcept -> std::chrono::milliseconds {
            constexpr double useful_quantum = 1024.0;
            const auto deficit = std::max(0.0, useful_quantum - tokens);
            const auto milliseconds = static_cast<std::int64_t>((deficit / bytes_per_second) * 1000.0) + 1;
            return std::chrono::milliseconds{std::max<std::int64_t>(10, milliseconds)};
        }
    };

    struct LineBucket final {
        double tokens{4.0};
        Clock::time_point updated{Clock::now()};

        auto take() -> bool {
            const auto now = Clock::now();
            tokens = std::min(4.0,
                tokens + std::chrono::duration<double>(now - updated).count());
            updated = now;
            if (tokens < 1.0) return false;
            tokens -= 1.0;
            return true;
        }
    };

    struct TargetBucket final {
        LineBucket bucket;
        std::uint64_t serial{};
    };

    static auto valid_options(const ConnectionOptions& options) noexcept -> bool {
        const auto proxy_invalid = options.proxy.kind != ProxyKind::none &&
            (!safe_network_name(options.proxy.host) || options.proxy.port == 0);
        const auto proxy_credentials_invalid =
            options.proxy.username.has_value() != options.proxy.password.has_value() ||
            (options.proxy.username && options.proxy.username->size() > 255) ||
            (options.proxy.password && options.proxy.password->size() > 255) ||
            (options.proxy.kind == ProxyKind::socks5 && options.proxy.username &&
             (options.proxy.username->empty() || options.proxy.password->empty())) ||
            (options.proxy.kind == ProxyKind::http_connect && options.proxy.username &&
             options.proxy.username->contains(':'));
        return safe_endpoint_host(options.endpoint.host) &&
            (options.security == Security::plaintext || safe_network_name(options.server_name)) &&
            (!options.ca_file || safe_path(*options.ca_file)) &&
            options.endpoint.port != 0 && !proxy_invalid && !proxy_credentials_invalid &&
            options.limits.receive_bytes != 0 && options.limits.transmit_bytes != 0 &&
            options.limits.queued_commands > event_control_headroom &&
            options.deadlines.connect > std::chrono::milliseconds::zero() &&
            options.deadlines.handshake > std::chrono::milliseconds::zero() &&
            options.deadlines.idle > std::chrono::milliseconds::zero() &&
            options.deadlines.ping > std::chrono::milliseconds::zero();
    }

    void reset_tls_configuration() noexcept {
        tls_established_ = false;
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_free(&tls_config_);
        mbedtls_ssl_config_init(&tls_config_);
        mbedtls_x509_crt_free(&ca_);
        mbedtls_x509_crt_init(&ca_);
        mbedtls_ctr_drbg_free(&random_);
        mbedtls_ctr_drbg_init(&random_);
        mbedtls_entropy_free(&entropy_);
        mbedtls_entropy_init(&entropy_);
        mbedtls_ssl_session_free(&session_);
        mbedtls_ssl_session_init(&session_);
        tls_configured_ = false;
        session_available_ = false;
        resume_offered_ = false;
        if (!resume_reference_.empty()) mbedtls_platform_zeroize(resume_reference_.data(), resume_reference_.size());
        resume_reference_.clear();
    }

    static auto serialize_session(const mbedtls_ssl_session& session) -> std::optional<std::vector<std::byte>> {
        std::size_t required{};
        const auto sizing = mbedtls_ssl_session_save(&session, nullptr, 0, &required);
        if ((sizing != 0 && sizing != MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL) || required == 0 || required > 64U * 1024U)
            return std::nullopt;
        std::vector<std::byte> output(required);
        std::size_t written{};
        if (mbedtls_ssl_session_save(&session, reinterpret_cast<unsigned char*>(output.data()),
                                     output.size(), &written) != 0) {
            mbedtls_platform_zeroize(output.data(), output.size());
            return std::nullopt;
        }
        output.resize(written);
        return output;
    }

    static auto constant_equal(const std::span<const std::byte> left,
                               const std::span<const std::byte> right) noexcept -> bool {
        if (left.size() != right.size()) return false;
        unsigned char difference{};
        for (std::size_t index = 0; index < left.size(); ++index)
            difference |= std::to_integer<unsigned char>(left[index] ^ right[index]);
        return difference == 0;
    }

    auto event_occupancy_locked() const noexcept -> std::size_t {
        return events_.size() + reserved_send_completions_ + reserved_receive_events_;
    }

    auto ordinary_event_capacity_locked() const noexcept -> std::size_t {
        return options_.limits.queued_commands - event_control_headroom;
    }

    auto publish(EventBody body) -> bool {
        std::function<void()> wakeup;
        {
            std::scoped_lock lock{mutex_};
            const bool completion = std::holds_alternative<SendComplete>(body);
            const auto* received = std::get_if<BytesReceived>(&body);
            const auto consume_reservation = [this, completion, received]() noexcept {
                if (completion) {
                    if (reserved_send_completions_ != 0) --reserved_send_completions_;
                } else if (received != nullptr) {
                    if (reserved_receive_events_ != 0) --reserved_receive_events_;
                }
            };
            if ((completion && reserved_send_completions_ == 0) ||
                (received != nullptr && reserved_receive_events_ == 0)) return false;
            if (stopped_) {
                consume_reservation();
                return false;
            }

            if (received == nullptr && !completion) {
                const bool critical = std::holds_alternative<Connected>(body) || std::holds_alternative<Closed>(body);
                const auto body_index = body.index();
                const auto same_kind = std::ranges::find_if(events_, [body_index](const Event& event) {
                    return !std::holds_alternative<BytesReceived>(event.body) &&
                        !std::holds_alternative<SendComplete>(event.body) && event.body.index() == body_index;
                });
                if (same_kind != events_.end()) events_.erase(same_kind);

                const auto capacity = critical ? options_.limits.queued_commands : ordinary_event_capacity_locked();
                if (event_occupancy_locked() >= capacity) {
                    if (!critical) return false;
                    const auto replaceable = std::ranges::find_if(events_, [](const Event& event) {
                        return !std::holds_alternative<BytesReceived>(event.body) &&
                            !std::holds_alternative<SendComplete>(event.body);
                    });
                    if (replaceable == events_.end()) return false;
                    events_.erase(replaceable);
                }
            }
            try {
                events_.push_back(Event{generation_, std::move(body)});
            } catch (...) {
                // The reservation belongs to this event even when deque growth
                // fails. Releasing it here keeps stop/restart and backpressure
                // accounting exact before the callback boundary fails closed.
                consume_reservation();
                throw;
            }
            if (received != nullptr && received->bytes)
                queued_receive_bytes_ += received->bytes->size();
            consume_reservation();
            auto peak = peak_queued_events_.load(std::memory_order_relaxed);
            while (peak < events_.size() &&
                   !peak_queued_events_.compare_exchange_weak(
                       peak, events_.size(), std::memory_order_relaxed, std::memory_order_relaxed)) {}
            wakeup = wakeup_;
        }
        if (wakeup) {
            try {
                wakeup();
            } catch (...) {
                // User notification is advisory; queued events remain intact.
            }
        }
        return true;
    }

    template <typename Function>
    static void callback_boundary(Impl* self, Function&& function) noexcept {
        try {
            function();
        } catch (...) {
            self->fail_noexcept("callback-exception", "network callback failed safely");
        }
    }

    auto take_command() -> std::optional<Command> {
        std::scoped_lock lock{mutex_};
        for (std::size_t pass = 0; pass < scheduling_order.size() * 2; ++pass) {
            if (scheduler_credit_ == 0) {
                scheduler_cursor_ = (scheduler_cursor_ + 1) % scheduling_order.size();
                scheduler_credit_ = scheduling_quanta[scheduler_cursor_];
            }
            auto& queue = commands_[scheduling_order[scheduler_cursor_]];
            if (!queue.empty()) {
                auto command = std::move(queue.front());
                queue.pop_front();
                --scheduler_credit_;
                return command;
            }
            scheduler_credit_ = 0;
        }
        return std::nullopt;
    }

    void release_queued_bytes(const std::size_t bytes) noexcept {
        std::scoped_lock lock{mutex_};
        queued_bytes_ = bytes > queued_bytes_ ? 0 : queued_bytes_ - bytes;
    }

    void release_canceled_send(const std::size_t bytes) noexcept {
        std::scoped_lock lock{mutex_};
        queued_bytes_ = bytes > queued_bytes_ ? 0 : queued_bytes_ - bytes;
        if (reserved_send_completions_ != 0) --reserved_send_completions_;
    }

    void clear_command_queues_locked() noexcept {
        for (auto& queue : commands_) {
            for (auto& command : queue) {
                if (auto* send = std::get_if<Send>(&command); send != nullptr && send->sensitive && !send->bytes.empty())
                    mbedtls_platform_zeroize(send->bytes.data(), send->bytes.size());
                if (std::holds_alternative<Send>(command) && reserved_send_completions_ != 0)
                    --reserved_send_completions_;
            }
            queue.clear();
        }
    }

    void cancel_pending_commands() noexcept {
        std::scoped_lock lock{mutex_};
        clear_command_queues_locked();
        queued_bytes_ = 0;
    }

    void process_command(Command command) {
        if (std::holds_alternative<Connect>(command)) {
            begin_resolve();
            return;
        }
        if (const auto* disconnect = std::get_if<Disconnect>(&command)) {
            intentional_close_ = true;
            cancel_pending_commands();
            close_transport();
            publish(StateChanged{State::stopped});
            publish(Closed{disconnect->reason, std::chrono::milliseconds{0}});
            {
                std::scoped_lock lock{mutex_};
                stopped_ = true;
                reserved_send_completions_ = 0;
                reserved_receive_events_ = 0;
            }
            thread_.request_stop();
            return;
        }
        auto send = std::get<Send>(std::move(command));
        PendingWrite pending{send.id, send.priority, std::move(send.bytes), 0,
            send.sensitive, std::move(send.target), false};
        const auto pending_size = pending.bytes.size();
        try {
            transmit_[static_cast<std::size_t>(pending.priority)].push_back(std::move(pending));
        } catch (...) {
            if (pending.sensitive && !pending.bytes.empty())
                mbedtls_platform_zeroize(pending.bytes.data(), pending.bytes.size());
            release_canceled_send(pending_size);
            throw;
        }
        if (pipeline_ == Pipeline::open) set_readiness((receive_paused_ ? 0 : UV_READABLE) | UV_WRITABLE);
    }

    void network_thread_entry(const threading::StopToken token) noexcept {
        try {
            network_loop(token);
        } catch (...) {
            fail_noexcept("network-exception", "network worker stopped after an internal exception");
            cleanup_network_loop();
        }
        std::scoped_lock lock{mutex_};
        stopped_ = true;
        clear_command_queues_locked();
        queued_bytes_ = 0;
        reserved_send_completions_ = 0;
        reserved_receive_events_ = 0;
        proxy_password_.reset();
    }

    auto initialize_network_loop() -> bool {
        if (uv_loop_init(&loop_) != 0) {
            publish(Diagnostic{"uv-loop-init", "network loop initialization failed"});
            return false;
        }
        loop_initialized_ = true;
        command_wakeup_.data = this;
        happy_timer_.data = this;
        reconnect_timer_.data = this;
        deadline_timer_.data = this;
        rate_timer_.data = this;
        if (uv_async_init(&loop_, &command_wakeup_, [](uv_async_t* wakeup) noexcept {
            auto* self = static_cast<Impl*>(wakeup->data);
            self->command_wakeups_.fetch_add(1, std::memory_order_relaxed);
        }) != 0) {
            publish(Diagnostic{"uv-async-init", "network command wakeup could not initialize"});
            return false;
        }
        command_wakeup_initialized_ = true;
        if (uv_timer_init(&loop_, &happy_timer_) != 0) {
            publish(Diagnostic{"uv-timer-init", "Happy Eyeballs timer could not initialize"});
            return false;
        }
        happy_timer_initialized_ = true;
        if (uv_timer_init(&loop_, &reconnect_timer_) != 0) {
            publish(Diagnostic{"uv-timer-init", "reconnect timer could not initialize"});
            return false;
        }
        reconnect_timer_initialized_ = true;
        if (uv_timer_init(&loop_, &deadline_timer_) != 0) {
            publish(Diagnostic{"uv-timer-init", "deadline timer could not initialize"});
            return false;
        }
        deadline_timer_initialized_ = true;
        if (uv_timer_init(&loop_, &rate_timer_) != 0) {
            publish(Diagnostic{"uv-timer-init", "rate timer could not initialize"});
            return false;
        }
        rate_timer_initialized_ = true;
        return true;
    }

    void network_loop(const threading::StopToken token) {
        if (!initialize_network_loop()) {
            cleanup_network_loop();
            std::scoped_lock lock{mutex_};
            stopped_ = true;
            return;
        }
        loop_ready_.store(true, std::memory_order_release);
        while (!token.stop_requested()) {
            while (auto command = take_command()) process_command(std::move(*command));
            (void)uv_run(&loop_, UV_RUN_ONCE);
            loop_iterations_.fetch_add(1, std::memory_order_relaxed);
            pump_pipeline();
        }
        cleanup_network_loop();
    }

    void cleanup_network_loop() noexcept {
        if (!loop_initialized_) {
            std::scoped_lock lock{mutex_};
            loop_ready_.store(false, std::memory_order_release);
            return;
        }
        // Close the cross-thread signaling gate while holding the same mutex
        // used by post(), poll(), and stop(). Once this releases, no caller can
        // enter uv_async_send before command_wakeup_ is closed below.
        {
            std::scoped_lock lock{mutex_};
            loop_ready_.store(false, std::memory_order_release);
        }
        intentional_close_ = true;
        cancel_resolve();
        close_transport();
        if (happy_timer_initialized_) uv_timer_stop(&happy_timer_);
        if (reconnect_timer_initialized_) uv_timer_stop(&reconnect_timer_);
        if (deadline_timer_initialized_) uv_timer_stop(&deadline_timer_);
        if (rate_timer_initialized_) uv_timer_stop(&rate_timer_);
        if (command_wakeup_initialized_ && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&command_wakeup_)))
            uv_close(reinterpret_cast<uv_handle_t*>(&command_wakeup_), nullptr);
        if (happy_timer_initialized_ && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&happy_timer_)))
            uv_close(reinterpret_cast<uv_handle_t*>(&happy_timer_), nullptr);
        if (reconnect_timer_initialized_ && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&reconnect_timer_)))
            uv_close(reinterpret_cast<uv_handle_t*>(&reconnect_timer_), nullptr);
        if (deadline_timer_initialized_ && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&deadline_timer_)))
            uv_close(reinterpret_cast<uv_handle_t*>(&deadline_timer_), nullptr);
        if (rate_timer_initialized_ && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&rate_timer_)))
            uv_close(reinterpret_cast<uv_handle_t*>(&rate_timer_), nullptr);
        uv_walk(&loop_, [](uv_handle_t* handle, void*) noexcept {
            if (!uv_is_closing(handle)) uv_close(handle, close_dynamic_handle);
        }, nullptr);
        while (uv_run(&loop_, UV_RUN_DEFAULT) != 0) {}
        (void)uv_loop_close(&loop_);
        loop_initialized_ = false;
        command_wakeup_initialized_ = false;
        happy_timer_initialized_ = false;
        reconnect_timer_initialized_ = false;
        deadline_timer_initialized_ = false;
        rate_timer_initialized_ = false;
    }

    static void close_dynamic_handle(uv_handle_t* handle) noexcept {
        if (handle->type == UV_TCP) delete static_cast<Attempt*>(handle->data);
    }

    static void readiness_closed(uv_handle_t* handle) noexcept {
        auto* self = static_cast<Impl*>(handle->data);
        self->readiness_initialized_ = false;
        self->readiness_closing_ = false;
    }

    static void readiness_callback(uv_poll_t* handle, const int status, int) noexcept {
        auto* self = static_cast<Impl*>(handle->data);
        callback_boundary(self, [&] {
            if (status < 0) self->fail_connection("socket-poll", "socket readiness monitoring failed");
            else self->pump_pipeline();
        });
    }

    void set_readiness(const int events) {
        if (!has_socket_ || readiness_closing_) return;
        if (!readiness_initialized_) {
            readiness_.data = this;
#if defined(_WIN32)
            if (uv_poll_init_socket(&loop_, &readiness_, reinterpret_cast<uv_os_sock_t>(socket_)) != 0) {
#else
            if (uv_poll_init(&loop_, &readiness_, socket_) != 0) {
#endif
                fail_connection("socket-poll", "socket readiness monitoring could not start");
                return;
            }
            readiness_initialized_ = true;
        }
        if (events == 0) {
            (void)uv_poll_stop(&readiness_);
            return;
        }
        if (uv_poll_start(&readiness_, events, readiness_callback) != 0)
            fail_connection("socket-poll", "socket readiness monitoring could not start");
    }

    void arm_deadline(const std::chrono::milliseconds duration) {
        if (!deadline_timer_initialized_) return;
        uv_timer_stop(&deadline_timer_);
        (void)uv_timer_start(&deadline_timer_, [](uv_timer_t* timer) noexcept {
            auto* self = static_cast<Impl*>(timer->data);
            callback_boundary(self, [&] { self->pump_pipeline(); });
        }, static_cast<std::uint64_t>(std::max<std::int64_t>(1, duration.count())), 0);
    }

    void begin_resolve() {
        if (!loop_ready_ || (pipeline_ != Pipeline::idle && pipeline_ != Pipeline::reconnect_wait)) return;
        if (resolver_ != nullptr) {
            resolve_restart_pending_ = pipeline_ == Pipeline::reconnect_wait;
            return;
        }
        intentional_close_ = false;
        resolve_restart_pending_ = false;
        addresses_.clear();
        next_address_ = 0;
        connect_started_ = Clock::now();
        arm_deadline(options_.deadlines.connect);
        const auto use_proxy = options_.proxy.kind != ProxyKind::none;
        resolve_host_ = use_proxy ? options_.proxy.host : options_.endpoint.host;
        resolve_service_ = std::to_string(use_proxy ? options_.proxy.port : options_.endpoint.port);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        auto* resolver = new ResolveRequest{};
        resolver->owner = this;
        resolver->serial = ++resolve_serial_;
        resolver->request.data = resolver;
        resolver_ = resolver;
        pipeline_ = Pipeline::resolving;
        publish(StateChanged{State::resolving});
        if (uv_getaddrinfo(&loop_, &resolver->request, resolve_callback,
                           resolve_host_.c_str(), resolve_service_.c_str(), &hints) != 0) {
            resolver_ = nullptr;
            delete resolver;
            fail_connection("dns-start", "name resolution could not start");
        }
    }

    static void resolve_callback(uv_getaddrinfo_t* request, const int status, addrinfo* result) noexcept {
        auto* resolver = static_cast<ResolveRequest*>(request->data);
        auto* self = resolver->owner;
        const bool current = self->resolver_ == resolver;
        if (current) self->resolver_ = nullptr;
        const bool stale = !current || resolver->canceled || resolver->serial != self->resolve_serial_ ||
            self->pipeline_ != Pipeline::resolving;
        callback_boundary(self, [&] {
            if (stale) {
                if (self->resolve_restart_pending_ && self->pipeline_ == Pipeline::reconnect_wait)
                    self->begin_resolve();
                return;
            }
            if (status < 0 || result == nullptr) {
                self->fail_connection("dns-failed", "name resolution failed");
                return;
            }
            std::vector<sockaddr_storage> ipv6;
            std::vector<sockaddr_storage> ipv4;
            for (auto* item = result; item != nullptr; item = item->ai_next) {
                if (item->ai_addr == nullptr || (item->ai_family != AF_INET && item->ai_family != AF_INET6)) continue;
                sockaddr_storage address{};
                std::memcpy(&address, item->ai_addr,
                            item->ai_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in));
                (item->ai_family == AF_INET6 ? ipv6 : ipv4).push_back(address);
            }
            const auto count = std::max(ipv6.size(), ipv4.size());
            for (std::size_t index = 0; index < count; ++index) {
                if (index < ipv6.size()) self->addresses_.push_back(ipv6[index]);
                if (index < ipv4.size()) self->addresses_.push_back(ipv4[index]);
            }
            if (self->addresses_.empty()) {
                self->fail_connection("dns-empty", "name resolution returned no usable address");
                return;
            }
            self->pipeline_ = Pipeline::connecting;
            self->publish(StateChanged{State::connecting});
            self->start_next_attempt();
            if (self->next_address_ < self->addresses_.size())
                (void)uv_timer_start(&self->happy_timer_, happy_timer_callback,
                                     static_cast<std::uint64_t>(happy_eyeballs_delay.count()),
                                     static_cast<std::uint64_t>(happy_eyeballs_delay.count()));
        });
        if (result != nullptr) uv_freeaddrinfo(result);
        delete resolver;
    }

    void start_next_attempt() {
        if (pipeline_ != Pipeline::connecting || next_address_ >= addresses_.size() || winner_ != nullptr) return;
        auto* attempt = new Attempt{};
        attempt->owner = this;
        attempt->tcp.data = attempt;
        attempt->request.data = attempt;
        if (uv_tcp_init(&loop_, &attempt->tcp) != 0) {
            delete attempt;
            fail_connection("tcp-init", "TCP initialization failed");
            return;
        }
        attempts_.push_back(attempt);
        const auto* address = reinterpret_cast<const sockaddr*>(&addresses_[next_address_++]);
        const auto status = uv_tcp_connect(&attempt->request, &attempt->tcp, address, connect_callback);
        if (status != 0) on_connect(attempt, status);
    }

    static void happy_timer_callback(uv_timer_t* timer) noexcept {
        auto* self = static_cast<Impl*>(timer->data);
        callback_boundary(self, [&] {
            self->start_next_attempt();
            if (self->next_address_ >= self->addresses_.size()) uv_timer_stop(timer);
        });
    }

    static void connect_callback(uv_connect_t* request, const int status) noexcept {
        auto* attempt = static_cast<Attempt*>(request->data);
        auto* self = attempt->owner;
        callback_boundary(self, [&] { self->on_connect(attempt, status); });
    }

    void on_connect(Attempt* attempt, const int status) {
        if (status == 0 && pipeline_ == Pipeline::connecting && winner_ == nullptr) {
            winner_ = attempt;
            uv_timer_stop(&happy_timer_);
            (void)uv_tcp_nodelay(&winner_->tcp, 1);
            if (uv_fileno(reinterpret_cast<uv_handle_t*>(&winner_->tcp), &socket_) != 0) {
                fail_connection("socket-handle", "connected socket was unavailable");
                return;
            }
            has_socket_ = true;
            set_readiness(UV_READABLE | UV_WRITABLE);
            for (auto* other : attempts_) if (other != winner_) close_attempt(other);
            attempts_.erase(std::remove_if(attempts_.begin(), attempts_.end(),
                [this](const auto* value) { return value != winner_; }), attempts_.end());
            if (options_.proxy.kind == ProxyKind::none) begin_tls_or_open();
            else begin_proxy();
            return;
        }
        attempts_.erase(std::remove(attempts_.begin(), attempts_.end(), attempt), attempts_.end());
        close_attempt(attempt);
        if (pipeline_ == Pipeline::connecting) {
            if (next_address_ < addresses_.size()) start_next_attempt();
            else if (attempts_.empty()) fail_connection("connect-failed", "all connection attempts failed");
        }
    }

    static void close_attempt(Attempt* attempt) noexcept {
        if (attempt == nullptr) return;
        auto* handle = reinterpret_cast<uv_handle_t*>(&attempt->tcp);
        if (!uv_is_closing(handle)) uv_close(handle, close_dynamic_handle);
    }

    void begin_proxy() {
        pipeline_ = Pipeline::proxy;
        clear_proxy_input();
        clear_proxy_output();
        clear_tunnel_input();
        if (!resume_reference_.empty()) mbedtls_platform_zeroize(resume_reference_.data(), resume_reference_.size());
        resume_reference_.clear();
        resume_offered_ = false;
        proxy_offset_ = 0;
        handshake_started_ = Clock::now();
        arm_deadline(options_.deadlines.handshake);
        publish(StateChanged{State::proxy_handshake});
        if (options_.proxy.kind == ProxyKind::socks5) {
            proxy_phase_ = ProxyPhase::socks_greeting;
            proxy_output_ = {std::byte{5}, std::byte{static_cast<unsigned char>(proxy_password_ ? 2 : 1)}, std::byte{0}};
            if (proxy_password_) proxy_output_.push_back(std::byte{2});
        } else {
            proxy_phase_ = ProxyPhase::http_connect;
            const auto address = http_authority(options_.endpoint);
            std::string request = "CONNECT " + address + " HTTP/1.1\r\nHost: " + address + "\r\n";
            if (options_.proxy.username && proxy_password_) {
                std::string credentials = *options_.proxy.username + ':';
                const auto password = proxy_password_->view();
                credentials.append(reinterpret_cast<const char*>(password.data()), password.size());
                std::size_t encoded_size{};
                (void)mbedtls_base64_encode(nullptr, 0, &encoded_size,
                    reinterpret_cast<const unsigned char*>(credentials.data()), credentials.size());
                std::string encoded(encoded_size, '\0');
                std::size_t written{};
                if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded.data()), encoded.size(), &written,
                    reinterpret_cast<const unsigned char*>(credentials.data()), credentials.size()) == 0) {
                    encoded.resize(written);
                    request += "Proxy-Authorization: Basic " + encoded + "\r\n";
                }
                secure_clear(credentials);
                secure_clear(encoded);
            }
            request += "Proxy-Connection: Keep-Alive\r\n\r\n";
            proxy_output_.resize(request.size());
            std::memcpy(proxy_output_.data(), request.data(), request.size());
            secure_clear(request);
        }
        set_readiness(UV_READABLE | UV_WRITABLE);
    }

    void pump_proxy() {
        if (proxy_offset_ < proxy_output_.size()) {
            const auto sent = raw_send(proxy_output_.data() + proxy_offset_, proxy_output_.size() - proxy_offset_);
            if (sent < 0) {
                if (!would_block()) fail_connection("proxy-write", "proxy negotiation write failed");
                return;
            }
            proxy_offset_ += static_cast<std::size_t>(sent);
            if (proxy_offset_ < proxy_output_.size()) return;
            set_readiness(UV_READABLE);
        }
        std::array<std::byte, 2048> input{};
        const auto received = raw_receive(input.data(), input.size());
        if (received == 0) {
            fail_connection("proxy-closed", "proxy closed during negotiation");
            return;
        }
        if (received < 0) {
            if (!would_block()) fail_connection("proxy-read", "proxy negotiation read failed");
            return;
        }
        if (proxy_input_.size() + static_cast<std::size_t>(received) > proxy_reply_limit) {
            fail_connection("proxy-oversize", "proxy negotiation exceeded its bound");
            return;
        }
        proxy_input_.insert(proxy_input_.end(), input.begin(), input.begin() + received);
        if (options_.proxy.kind == ProxyKind::socks5) pump_socks_reply();
        else pump_http_reply();
    }

    void queue_socks_connect() {
        proxy_phase_ = ProxyPhase::socks_connect;
        clear_proxy_input();
        clear_proxy_output();
        proxy_output_ = {std::byte{5}, std::byte{1}, std::byte{0}};
        sockaddr_in ipv4{};
        sockaddr_in6 ipv6{};
        if (uv_ip4_addr(options_.endpoint.host.c_str(), 0, &ipv4) == 0) {
            proxy_output_.push_back(std::byte{1});
            const auto* address = reinterpret_cast<const std::byte*>(&ipv4.sin_addr);
            proxy_output_.insert(proxy_output_.end(), address, address + sizeof(ipv4.sin_addr));
        } else if (uv_ip6_addr(options_.endpoint.host.c_str(), 0, &ipv6) == 0) {
            proxy_output_.push_back(std::byte{4});
            const auto* address = reinterpret_cast<const std::byte*>(&ipv6.sin6_addr);
            proxy_output_.insert(proxy_output_.end(), address, address + sizeof(ipv6.sin6_addr));
        } else {
            if (options_.endpoint.host.size() > 255) {
                fail_connection("proxy-target", "proxy target name is too long");
                return;
            }
            proxy_output_.push_back(std::byte{3});
            proxy_output_.push_back(std::byte{
                static_cast<unsigned char>(options_.endpoint.host.size())});
            for (const auto character : options_.endpoint.host)
                proxy_output_.push_back(std::byte{static_cast<unsigned char>(character)});
        }
        proxy_output_.push_back(std::byte{static_cast<unsigned char>(options_.endpoint.port >> 8U)});
        proxy_output_.push_back(std::byte{static_cast<unsigned char>(options_.endpoint.port & 0xffU)});
        proxy_offset_ = 0;
        set_readiness(UV_READABLE | UV_WRITABLE);
    }

    void pump_socks_reply() {
        if (proxy_phase_ == ProxyPhase::socks_greeting) {
            if (proxy_input_.size() < 2) return;
            if (proxy_input_[0] != std::byte{5}) {
                fail_connection("proxy-protocol", "SOCKS version mismatch");
                return;
            }
            if (proxy_input_[1] == std::byte{0}) {
                queue_socks_connect();
                return;
            }
            if (proxy_input_[1] != std::byte{2} || !options_.proxy.username || !proxy_password_ ||
                options_.proxy.username->size() > 255 || proxy_password_->view().size() > 255) {
                fail_connection("proxy-auth", "SOCKS authentication negotiation failed");
                return;
            }
            proxy_phase_ = ProxyPhase::socks_auth;
            clear_proxy_input();
            clear_proxy_output();
            proxy_output_ = {std::byte{1}, std::byte{static_cast<unsigned char>(options_.proxy.username->size())}};
            for (const auto character : *options_.proxy.username)
                proxy_output_.push_back(std::byte{static_cast<unsigned char>(character)});
            const auto password = proxy_password_->view();
            proxy_output_.push_back(std::byte{static_cast<unsigned char>(password.size())});
            proxy_output_.insert(proxy_output_.end(), password.begin(), password.end());
            proxy_offset_ = 0;
            set_readiness(UV_READABLE | UV_WRITABLE);
            return;
        }
        if (proxy_phase_ == ProxyPhase::socks_auth) {
            if (proxy_input_.size() < 2) return;
            if (proxy_input_[0] != std::byte{1} || proxy_input_[1] != std::byte{0}) {
                fail_connection("proxy-auth", "SOCKS authentication failed");
                return;
            }
            queue_socks_connect();
            return;
        }
        if (proxy_input_.size() < 5) return;
        if (proxy_input_[0] != std::byte{5} || proxy_input_[1] != std::byte{0} ||
            proxy_input_[2] != std::byte{0}) {
            fail_connection("proxy-connect", "SOCKS target connection failed");
            return;
        }
        std::size_t expected{};
        if (proxy_input_[3] == std::byte{1}) expected = 10;
        else if (proxy_input_[3] == std::byte{4}) expected = 22;
        else if (proxy_input_[3] == std::byte{3} && proxy_input_.size() >= 5)
            expected = 7 + std::to_integer<unsigned char>(proxy_input_[4]);
        else {
            fail_connection("proxy-protocol", "SOCKS reply address was invalid");
            return;
        }
        if (proxy_input_.size() >= expected) finish_proxy(expected);
    }

    void pump_http_reply() {
        const std::string_view reply{reinterpret_cast<const char*>(proxy_input_.data()), proxy_input_.size()};
        const auto header_end = reply.find("\r\n\r\n");
        if (header_end == std::string_view::npos) return;
        const auto line_end = reply.find("\r\n");
        if (line_end == std::string_view::npos) return;
        const auto first_line = reply.substr(0, line_end);
        const bool version = first_line.starts_with("HTTP/1.0 ") || first_line.starts_with("HTTP/1.1 ");
        if (!version || first_line.size() < 12 || first_line.substr(9, 3) != "200" ||
            (first_line.size() > 12 && first_line[12] != ' ')) {
            fail_connection("proxy-connect", "HTTP CONNECT proxy rejected the tunnel");
            return;
        }
        finish_proxy(header_end + 4);
    }

    void finish_proxy(const std::size_t consumed) {
        clear_tunnel_input();
        if (consumed < proxy_input_.size()) {
            tunnel_input_.assign(proxy_input_.begin() + static_cast<std::ptrdiff_t>(consumed), proxy_input_.end());
        }
        clear_proxy_input();
        begin_tls_or_open();
    }

    void clear_proxy_input() noexcept {
        if (!proxy_input_.empty()) mbedtls_platform_zeroize(proxy_input_.data(), proxy_input_.size());
        proxy_input_.clear();
    }

    void clear_proxy_output() noexcept {
        if (!proxy_output_.empty()) mbedtls_platform_zeroize(proxy_output_.data(), proxy_output_.size());
        proxy_output_.clear();
    }

    void clear_tunnel_input() noexcept {
        if (!tunnel_input_.empty()) mbedtls_platform_zeroize(tunnel_input_.data(), tunnel_input_.size());
        tunnel_input_.clear();
        tunnel_offset_ = 0;
    }

    void mark_server_activity() {
        last_activity_ = Clock::now();
        ping_notified_ = false;
        arm_deadline(std::min(options_.deadlines.ping, options_.deadlines.idle));
    }

    void begin_tls_or_open() {
        clear_proxy_output();
        if (options_.security == Security::plaintext) {
            pipeline_ = Pipeline::open;
            receive_paused_ = false;
            mark_server_activity();
            set_readiness(UV_READABLE);
            reconnect_failures_ = 0;
            publish(StateChanged{State::connected});
            publish(Connected{options_.endpoint.host, local_address(), false, false});
            return;
        }
        if (!configure_tls()) {
            fail_connection("tls-config", "TLS trust configuration failed");
            return;
        }
        // A fresh context carries no session, so the flag tracks it from here;
        // the handshake completing is what makes close_notify sendable again.
        tls_established_ = false;
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_init(&ssl_);
        if (mbedtls_ssl_setup(&ssl_, &tls_config_) != 0 ||
            mbedtls_ssl_set_hostname(&ssl_, options_.server_name.c_str()) != 0) {
            fail_connection("tls-setup", "TLS client setup failed");
            return;
        }
        resume_offered_ = false;
        if (!resume_reference_.empty()) mbedtls_platform_zeroize(resume_reference_.data(), resume_reference_.size());
        resume_reference_.clear();
        if (options_.enable_session_resumption && session_available_) {
            if (auto serialized = serialize_session(session_)) resume_reference_ = std::move(*serialized);
            resume_offered_ = !resume_reference_.empty() && mbedtls_ssl_set_session(&ssl_, &session_) == 0;
        }
        mbedtls_ssl_set_bio(&ssl_, this, tls_send_callback, tls_receive_callback, nullptr);
        pipeline_ = Pipeline::tls;
        handshake_started_ = Clock::now();
        arm_deadline(options_.deadlines.handshake);
        set_readiness(UV_READABLE | UV_WRITABLE);
        publish(StateChanged{State::tls_handshake});
    }

    auto configure_tls() -> bool {
        if (tls_configured_) return true;
        if (!crypto_ready_) return false;
        static constexpr unsigned char personalization[] = "comic-chat-transport-v1";
        if (mbedtls_ctr_drbg_seed(&random_, mbedtls_entropy_func, &entropy_, personalization,
                                  sizeof(personalization) - 1) != 0 ||
            mbedtls_ssl_config_defaults(&tls_config_, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            return false;
        }
        bool trust_loaded{};
        if (options_.ca_file) {
            trust_loaded = mbedtls_x509_crt_parse_file(&ca_, options_.ca_file->c_str()) == 0;
        } else {
#if defined(_WIN32)
            const auto store = CertOpenSystemStoreW(0, L"ROOT");
            if (store != nullptr) {
                PCCERT_CONTEXT certificate{};
                while ((certificate = CertEnumCertificatesInStore(store, certificate)) != nullptr) {
                    if (mbedtls_x509_crt_parse_der(&ca_, certificate->pbCertEncoded,
                                                   certificate->cbCertEncoded) == 0)
                        trust_loaded = true;
                }
                (void)CertCloseStore(store, 0);
            }
#else
            static constexpr std::string_view candidates[]{
                "/etc/ssl/certs/ca-certificates.crt",
                "/etc/ssl/cert.pem",
                "/etc/pki/tls/certs/ca-bundle.crt",
                "/usr/local/share/certs/ca-root-nss.crt",
            };
            std::error_code filesystem_error;
            for (const auto candidate : candidates) {
                filesystem_error.clear();
                if (std::filesystem::is_regular_file(candidate, filesystem_error) && !filesystem_error &&
                    mbedtls_x509_crt_parse_file(&ca_, std::string{candidate}.c_str()) == 0) {
                    trust_loaded = true;
                    break;
                }
            }
            filesystem_error.clear();
            if (!trust_loaded && std::filesystem::is_directory("/etc/ssl/certs", filesystem_error) && !filesystem_error)
                trust_loaded = mbedtls_x509_crt_parse_path(&ca_, "/etc/ssl/certs") >= 0 && ca_.version != 0;
#endif
        }
        if (!trust_loaded) return false;
        mbedtls_ssl_conf_rng(&tls_config_, mbedtls_ctr_drbg_random, &random_);
        mbedtls_ssl_conf_ca_chain(&tls_config_, &ca_, nullptr);
        mbedtls_ssl_conf_authmode(&tls_config_, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_min_tls_version(&tls_config_, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
        mbedtls_ssl_conf_session_tickets(&tls_config_, options_.enable_session_resumption ?
            MBEDTLS_SSL_SESSION_TICKETS_ENABLED : MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif
        tls_configured_ = true;
        return true;
    }

    static auto tls_send_callback(void* context, const unsigned char* data, const std::size_t size) -> int {
        auto* self = static_cast<Impl*>(context);
        const auto sent = self->raw_send(reinterpret_cast<const std::byte*>(data), size);
        if (sent >= 0) return static_cast<int>(sent);
        return self->would_block() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    }

    static auto tls_receive_callback(void* context, unsigned char* data, const std::size_t size) -> int {
        auto* self = static_cast<Impl*>(context);
        const auto received = self->raw_receive(reinterpret_cast<std::byte*>(data), size);
        if (received > 0) return static_cast<int>(received);
        if (received == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
        return self->would_block() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    }

    void pump_tls() {
        const auto status = mbedtls_ssl_handshake(&ssl_);
        if (status == 0) {
            if (mbedtls_ssl_get_verify_result(&ssl_) != 0) {
                fail_connection("tls-verify", "TLS peer verification failed");
                return;
            }
            bool resumed{};
            if (options_.enable_session_resumption) {
                mbedtls_ssl_session_free(&session_);
                mbedtls_ssl_session_init(&session_);
                session_available_ = mbedtls_ssl_get_session(&ssl_, &session_) == 0;
                if (session_available_) {
                    if (auto current = serialize_session(session_)) {
                        resumed = resume_offered_ && constant_equal(resume_reference_, *current);
                        mbedtls_platform_zeroize(current->data(), current->size());
                    }
                }
            }
            if (!resume_reference_.empty()) mbedtls_platform_zeroize(resume_reference_.data(), resume_reference_.size());
            resume_reference_.clear();
            resume_offered_ = false;
            tls_established_ = true;
            pipeline_ = Pipeline::open;
            receive_paused_ = false;
            mark_server_activity();
            set_readiness(UV_READABLE);
            reconnect_failures_ = 0;
            publish(StateChanged{State::connected});
            publish(Connected{options_.endpoint.host, local_address(), true, resumed});
            return;
        }
        if (status == MBEDTLS_ERR_SSL_WANT_READ) set_readiness(UV_READABLE);
        else if (status == MBEDTLS_ERR_SSL_WANT_WRITE) set_readiness(UV_READABLE | UV_WRITABLE);
        else
            fail_connection("tls-handshake", "TLS handshake or hostname verification failed");
    }

    void pump_open() {
        pump_writes();
        if (pipeline_ != Pipeline::open) return;
        std::array<std::byte, io_chunk> buffer{};
        std::size_t total{};
        while (total < options_.limits.receive_bytes) {
            const auto credit = reserve_receive_credit();
            if (credit == 0) {
                receive_paused_ = true;
                set_readiness(has_pending_writes() ? UV_WRITABLE : 0);
                return;
            }
            receive_paused_ = false;
            const auto maximum = std::min({buffer.size(), options_.limits.receive_bytes - total, credit});
            const auto received = options_.security == Security::tls
                ? mbedtls_ssl_read(&ssl_, reinterpret_cast<unsigned char*>(buffer.data()), maximum)
                : raw_receive(buffer.data(), maximum);
            if (received > 0) {
                total += static_cast<std::size_t>(received);
                mark_server_activity();
                try {
                    auto bytes = std::make_shared<std::vector<std::byte>>(buffer.begin(), buffer.begin() + received);
                    publish(BytesReceived{std::move(bytes)});
                } catch (...) {
                    // Allocation may fail before publish can consume the
                    // receive reservation.
                    release_receive_reservation();
                    throw;
                }
                continue;
            }
            release_receive_reservation();
            if (options_.security == Security::tls &&
                (received == MBEDTLS_ERR_SSL_WANT_READ || received == MBEDTLS_ERR_SSL_WANT_WRITE)) {
                set_readiness(UV_READABLE | (received == MBEDTLS_ERR_SSL_WANT_WRITE ? UV_WRITABLE : 0));
                break;
            }
            if (options_.security == Security::plaintext && received < 0 && would_block()) {
                set_readiness(UV_READABLE | (has_pending_writes() ? UV_WRITABLE : 0));
                break;
            }
            if (received == 0 || received == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                fail_connection("peer-closed", "peer closed the connection");
                break;
            }
            fail_connection("read-failed", "connection read failed");
            break;
        }
    }

    auto reserve_receive_credit() noexcept -> std::size_t {
        std::scoped_lock lock{mutex_};
        if (event_occupancy_locked() >= ordinary_event_capacity_locked() ||
            queued_receive_bytes_ >= options_.limits.receive_bytes) return 0;
        ++reserved_receive_events_;
        return options_.limits.receive_bytes - queued_receive_bytes_;
    }

    void release_receive_reservation() noexcept {
        std::scoped_lock lock{mutex_};
        if (reserved_receive_events_ != 0) --reserved_receive_events_;
    }

    auto has_pending_writes() const noexcept -> bool {
        return std::ranges::any_of(transmit_, [](const auto& queue) { return !queue.empty(); });
    }

    void pump_writes() {
        bool target_throttled{};
        for (std::size_t pass = 0; pass < scheduling_order.size() * 2; ++pass) {
            if (transmit_credit_ == 0) {
                transmit_cursor_ = (transmit_cursor_ + 1) % scheduling_order.size();
                transmit_credit_ = scheduling_quanta[transmit_cursor_];
            }
            const auto queue_index = scheduling_order[transmit_cursor_];
            auto& queue = transmit_[queue_index];
            if (queue.empty()) {
                transmit_credit_ = 0;
                continue;
            }
            auto& pending = queue.front();
            if (pending.priority == Priority::chat && pending.offset == 0 &&
                !pending.target.empty() && !pending.target_token_charged) {
                auto target = pending.target;
                std::ranges::transform(target, target.begin(), [](const unsigned char byte) {
                    return static_cast<char>(std::tolower(byte));
                });
                auto found = target_buckets_.find(target);
                if (found == target_buckets_.end()) {
                    if (target_buckets_.size() >= 128) {
                        const auto oldest = std::ranges::min_element(target_buckets_, {}, [](const auto& item) {
                            return item.second.serial;
                        });
                        if (oldest != target_buckets_.end()) target_buckets_.erase(oldest);
                    }
                    found = target_buckets_.try_emplace(std::move(target)).first;
                }
                found->second.serial = ++target_serial_;
                if (!found->second.bucket.take()) {
                    queue.push_back(std::move(queue.front()));
                    queue.pop_front();
                    target_throttled = true;
                    target_throttle_deferrals_.fetch_add(1, std::memory_order_relaxed);
                    transmit_credit_ = 0;
                    continue;
                }
                pending.target_token_charged = true;
            }
            const auto remaining = pending.bytes.size() - pending.offset;
            auto amount = remaining;
            TokenBucket* bucket{};
            double bucket_rate{};
            if (pending.priority == Priority::chat) {
                bucket = &chat_bucket_;
                bucket_rate = 32U * 1024U;
                amount = bucket->allowance(remaining, bucket_rate);
            } else if (pending.priority == Priority::bulk) {
                bucket = &bulk_bucket_;
                bucket_rate = 16U * 1024U;
                amount = bucket->allowance(remaining, bucket_rate);
            }
            if (amount == 0) {
                set_readiness(receive_paused_ ? 0 : UV_READABLE);
                const auto delay = bucket == nullptr ? std::chrono::milliseconds{10} : bucket->refill_delay(bucket_rate);
                (void)uv_timer_start(&rate_timer_, [](uv_timer_t* timer) noexcept {
                    auto* self = static_cast<Impl*>(timer->data);
                    callback_boundary(self, [&] {
                        if (self->pipeline_ == Pipeline::open) {
                            self->set_readiness((self->receive_paused_ ? 0 : UV_READABLE) | UV_WRITABLE);
                            self->pump_writes();
                        }
                    });
                }, static_cast<std::uint64_t>(delay.count()), 0);
                return;
            }
            const auto written = options_.security == Security::tls
                ? mbedtls_ssl_write(&ssl_, reinterpret_cast<const unsigned char*>(pending.bytes.data() + pending.offset), amount)
                : raw_send(pending.bytes.data() + pending.offset, amount);
            if (written > 0) {
                pending.offset += static_cast<std::size_t>(written);
                if (bucket != nullptr) bucket->consume(static_cast<std::size_t>(written));
                --transmit_credit_;
                if (pending.offset == pending.bytes.size()) {
                    const auto id = pending.id;
                    const auto byte_count = pending.bytes.size();
                    if (pending.sensitive) mbedtls_platform_zeroize(pending.bytes.data(), pending.bytes.size());
                    queue.pop_front();
                    release_queued_bytes(byte_count);
                    publish(SendComplete{id});
                }
                set_readiness((receive_paused_ ? 0 : UV_READABLE) |
                              (has_pending_writes() ? UV_WRITABLE : 0));
                return;
            }
            if (options_.security == Security::tls && written == MBEDTLS_ERR_SSL_WANT_READ) {
                set_readiness(receive_paused_ ? 0 : UV_READABLE);
                return;
            }
            if (options_.security == Security::tls && written == MBEDTLS_ERR_SSL_WANT_WRITE) {
                set_readiness((receive_paused_ ? 0 : UV_READABLE) | UV_WRITABLE);
                return;
            }
            if (options_.security == Security::plaintext && written < 0 && would_block()) {
                set_readiness((receive_paused_ ? 0 : UV_READABLE) | UV_WRITABLE);
                return;
            }
            fail_connection("write-failed", "connection write failed");
            return;
        }
        if (target_throttled && rate_timer_initialized_) {
            set_readiness(receive_paused_ ? 0 : UV_READABLE);
            (void)uv_timer_start(&rate_timer_, [](uv_timer_t* timer) noexcept {
                auto* self = static_cast<Impl*>(timer->data);
                callback_boundary(self, [&] {
                    if (self->pipeline_ == Pipeline::open) {
                        self->set_readiness((self->receive_paused_ ? 0 : UV_READABLE) | UV_WRITABLE);
                        self->pump_writes();
                    }
                });
            }, 50, 0);
        }
    }

    void pump_pipeline() {
        const auto now = Clock::now();
        if (pipeline_ == Pipeline::resolving || pipeline_ == Pipeline::connecting) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - connect_started_);
            if (elapsed >= options_.deadlines.connect) {
                fail_connection("connect-timeout", "connection deadline expired");
                return;
            }
            arm_deadline(options_.deadlines.connect - elapsed);
        }
        if (pipeline_ == Pipeline::proxy || pipeline_ == Pipeline::tls) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - handshake_started_);
            if (elapsed >= options_.deadlines.handshake) {
                fail_connection("handshake-timeout", "handshake deadline expired");
                return;
            }
            arm_deadline(options_.deadlines.handshake - elapsed);
        }
        if (pipeline_ == Pipeline::open) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_activity_);
            if (elapsed >= options_.deadlines.idle) {
                fail_connection("idle-timeout", "idle deadline expired");
                return;
            }
            if (!ping_notified_ && elapsed >= options_.deadlines.ping)
                ping_notified_ = publish(PingDue{});
            auto remaining = options_.deadlines.idle - elapsed;
            if (!ping_notified_) {
                const auto until_ping = options_.deadlines.ping - elapsed;
                // A saturated event queue must not create a 1ms timer spin.
                remaining = std::min(remaining, until_ping > std::chrono::milliseconds::zero()
                    ? until_ping : std::chrono::milliseconds{250});
            }
            arm_deadline(remaining);
        }
        if (pipeline_ == Pipeline::proxy) pump_proxy();
        else if (pipeline_ == Pipeline::tls) pump_tls();
        else if (pipeline_ == Pipeline::open) pump_open();
    }

    void fail_connection(std::string code, std::string message) {
        if (pipeline_ == Pipeline::idle || pipeline_ == Pipeline::reconnect_wait) return;
        cancel_resolve();
        close_transport();
        if (intentional_close_) return;
        const auto delay = retry_delay(reconnect_failures_++, reconnect_jitter_seed_);
        pipeline_ = Pipeline::reconnect_wait;
        publish(Diagnostic{std::move(code), std::move(message)});
        publish(StateChanged{State::reconnect_wait});
        publish(Closed{"transport failure", delay});
        (void)uv_timer_start(&reconnect_timer_, reconnect_timer_callback,
                             static_cast<std::uint64_t>(delay.count()), 0);
    }

    static void reconnect_timer_callback(uv_timer_t* timer) noexcept {
        auto* self = static_cast<Impl*>(timer->data);
        callback_boundary(self, [&] {
            if (!self->intentional_close_ && self->pipeline_ == Pipeline::reconnect_wait) self->begin_resolve();
        });
    }

    void fail_noexcept(const char* code, const char* message) noexcept {
        // This path is called from C callbacks. It cannot allow another
        // exception to cross libuv, and it deliberately stops instead of
        // retrying partially-mutated callback state.
        cancel_resolve();
        close_transport();
        cancel_pending_commands();
        try {
            (void)publish(Diagnostic{code, message});
            (void)publish(StateChanged{State::stopped});
            (void)publish(Closed{"transport callback failure", std::chrono::milliseconds{0}});
        } catch (...) {
        }
        intentional_close_ = true;
        pipeline_ = Pipeline::idle;
        thread_.request_stop();
        std::scoped_lock lock{mutex_};
        stopped_ = true;
        queued_bytes_ = 0;
        reserved_send_completions_ = 0;
        reserved_receive_events_ = 0;
    }

    void cancel_resolve() noexcept {
        if (resolver_ != nullptr) {
            resolver_->canceled = true;
            (void)uv_cancel(reinterpret_cast<uv_req_t*>(&resolver_->request));
        }
    }

    void close_transport() noexcept {
        // close_notify may only be sent while the current socket still carries
        // the session those keys belong to. A context left established past its
        // socket would encrypt the alert under the old session and emit it onto
        // whatever socket the BIO now points at, such as a reconnect proxy.
        if (has_socket_ && tls_established_) (void)mbedtls_ssl_close_notify(&ssl_);
        if (happy_timer_initialized_) uv_timer_stop(&happy_timer_);
        if (deadline_timer_initialized_) uv_timer_stop(&deadline_timer_);
        if (rate_timer_initialized_) uv_timer_stop(&rate_timer_);
        if (readiness_initialized_ && !readiness_closing_) {
            (void)uv_poll_stop(&readiness_);
            readiness_closing_ = true;
            uv_close(reinterpret_cast<uv_handle_t*>(&readiness_), readiness_closed);
        }
        for (auto* attempt : attempts_) if (attempt != winner_) close_attempt(attempt);
        attempts_.clear();
        if (winner_ != nullptr) {
            close_attempt(winner_);
            winner_ = nullptr;
        }
        has_socket_ = false;
        // The ssl_ context is scoped to one socket. Returning it to a pristine
        // state here keeps the next attempt from inheriting this session; the
        // resumption ticket lives in session_ and is deliberately untouched.
        tls_established_ = false;
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_init(&ssl_);
        pipeline_ = Pipeline::idle;
        receive_paused_ = false;
        proxy_phase_ = ProxyPhase::none;
        clear_proxy_input();
        clear_proxy_output();
        clear_tunnel_input();
        if (!resume_reference_.empty()) mbedtls_platform_zeroize(resume_reference_.data(), resume_reference_.size());
        resume_reference_.clear();
        resume_offered_ = false;
        for (auto& queue : transmit_) {
            for (auto& pending : queue) {
                if (pending.sensitive && !pending.bytes.empty())
                    mbedtls_platform_zeroize(pending.bytes.data(), pending.bytes.size());
                release_canceled_send(pending.bytes.size());
            }
            queue.clear();
        }
    }

    auto raw_send(const std::byte* data, const std::size_t size) noexcept -> std::ptrdiff_t {
        if (!has_socket_) return -1;
        const auto amount = std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
#if defined(_WIN32)
        return ::send(reinterpret_cast<SOCKET>(socket_), reinterpret_cast<const char*>(data),
                      static_cast<int>(amount), 0);
#else
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        return ::send(socket_, data, amount, flags);
#endif
    }

    auto raw_receive(std::byte* data, const std::size_t size) noexcept -> std::ptrdiff_t {
        if (!has_socket_) return -1;
        if (tunnel_offset_ < tunnel_input_.size()) {
            const auto amount = std::min(size, tunnel_input_.size() - tunnel_offset_);
            std::memcpy(data, tunnel_input_.data() + tunnel_offset_, amount);
            mbedtls_platform_zeroize(tunnel_input_.data() + tunnel_offset_, amount);
            tunnel_offset_ += amount;
            if (tunnel_offset_ == tunnel_input_.size()) clear_tunnel_input();
            return static_cast<std::ptrdiff_t>(amount);
        }
        const auto amount = std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
#if defined(_WIN32)
        return ::recv(reinterpret_cast<SOCKET>(socket_), reinterpret_cast<char*>(data),
                      static_cast<int>(amount), 0);
#else
        return ::recv(socket_, data, amount, 0);
#endif
    }

    auto local_address() const -> std::string {
        if (!has_socket_) return {};
        sockaddr_storage address{};
#if defined(_WIN32)
        int size = sizeof(address);
        if (::getsockname(reinterpret_cast<SOCKET>(socket_), reinterpret_cast<sockaddr*>(&address), &size) != 0)
            return {};
#else
        socklen_t size = sizeof(address);
        if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size) != 0) return {};
#endif
        std::array<char, INET6_ADDRSTRLEN> text{};
        if (address.ss_family == AF_INET) {
            if (uv_ip4_name(reinterpret_cast<const sockaddr_in*>(&address), text.data(), text.size()) != 0) return {};
        } else if (address.ss_family == AF_INET6) {
            if (uv_ip6_name(reinterpret_cast<const sockaddr_in6*>(&address), text.data(), text.size()) != 0) return {};
        } else {
            return {};
        }
        return text.data();
    }

    static auto would_block() noexcept -> bool {
#if defined(_WIN32)
        const auto error = WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
    }

    mutable std::mutex mutex_;
    std::array<std::deque<Command>, 5> commands_;
    std::array<std::deque<PendingWrite>, 5> transmit_;
    std::deque<Event> events_;
    std::function<void()> wakeup_;
    ConnectionOptions options_;
    std::optional<LockedSecret> proxy_password_;
    threading::JThread thread_;
    GenerationId generation_{};
    std::size_t queued_bytes_{};
    std::size_t queued_receive_bytes_{};
    std::size_t reserved_send_completions_{};
    std::size_t reserved_receive_events_{};
    std::size_t scheduler_cursor_{};
    unsigned int scheduler_credit_{scheduling_quanta[0]};
    std::size_t transmit_cursor_{};
    unsigned int transmit_credit_{scheduling_quanta[0]};
    TokenBucket chat_bucket_;
    TokenBucket bulk_bucket_;
    std::map<std::string, TargetBucket> target_buckets_;
    std::uint64_t target_serial_{};
    bool stopped_{true};

    uv_loop_t loop_{};
    ResolveRequest* resolver_{};
    uv_async_t command_wakeup_{};
    uv_timer_t happy_timer_{};
    uv_timer_t reconnect_timer_{};
    uv_timer_t deadline_timer_{};
    uv_timer_t rate_timer_{};
    uv_poll_t readiness_{};
    std::vector<sockaddr_storage> addresses_;
    std::vector<Attempt*> attempts_;
    Attempt* winner_{};
    uv_os_fd_t socket_{};
    std::string resolve_host_;
    std::string resolve_service_;
    std::size_t next_address_{};
    std::atomic_bool loop_ready_{};
    std::atomic_uint64_t loop_iterations_{};
    std::atomic_uint64_t command_wakeups_{};
    std::atomic_uint64_t rejected_sensitive_bytes_wiped_{};
    std::atomic_uint64_t peak_queued_events_{};
    std::atomic_uint64_t target_throttle_deferrals_{};
    std::uint64_t resolve_serial_{};
    bool resolve_restart_pending_{};
    bool has_socket_{};
    bool tls_established_{};
    bool readiness_initialized_{};
    bool readiness_closing_{};
    bool intentional_close_{};
    bool receive_paused_{};
    bool loop_initialized_{};
    bool command_wakeup_initialized_{};
    bool happy_timer_initialized_{};
    bool reconnect_timer_initialized_{};
    bool deadline_timer_initialized_{};
    bool rate_timer_initialized_{};
    Pipeline pipeline_{Pipeline::idle};
    ProxyPhase proxy_phase_{ProxyPhase::none};
    std::vector<std::byte> proxy_input_;
    std::vector<std::byte> proxy_output_;
    std::size_t proxy_offset_{};
    std::vector<std::byte> tunnel_input_;
    std::size_t tunnel_offset_{};
    unsigned int reconnect_failures_{};
    const std::uint64_t entropy_jitter_seed_{entropy_seed()};
    std::uint64_t reconnect_jitter_seed_{entropy_jitter_seed_};
    Clock::time_point connect_started_{};
    Clock::time_point handshake_started_{};
    Clock::time_point last_activity_{};
    bool ping_notified_{};

    mbedtls_ssl_context ssl_{};
    mbedtls_ssl_config tls_config_{};
    mbedtls_x509_crt ca_{};
    mbedtls_ctr_drbg_context random_{};
    mbedtls_entropy_context entropy_{};
    mbedtls_ssl_session session_{};
    bool tls_configured_{};
    bool crypto_ready_{};
    bool session_available_{};
    bool resume_offered_{};
    std::vector<std::byte> resume_reference_;
};

ConnectionEngine::ConnectionEngine() : impl_{std::make_unique<Impl>()} {}
ConnectionEngine::~ConnectionEngine() = default;
ConnectionEngine::ConnectionEngine(ConnectionEngine&&) noexcept = default;
auto ConnectionEngine::operator=(ConnectionEngine&&) noexcept -> ConnectionEngine& = default;
auto ConnectionEngine::start(ConnectionOptions options) -> std::expected<GenerationId, EngineError> {
    if (!impl_) impl_ = std::make_unique<Impl>();
    return impl_->start(std::move(options));
}
auto ConnectionEngine::post(Command command) -> std::expected<void, EngineError> {
    if (!impl_) {
        if (auto* send = std::get_if<Send>(&command); send != nullptr && send->sensitive && !send->bytes.empty()) {
            mbedtls_platform_zeroize(send->bytes.data(), send->bytes.size());
            send->bytes.clear();
        }
        return std::unexpected{EngineError::not_running};
    }
    return impl_->post(std::move(command));
}
auto ConnectionEngine::poll_events(const std::size_t maximum) -> std::vector<Event> {
    return impl_ ? impl_->poll(maximum) : std::vector<Event>{};
}
void ConnectionEngine::set_wakeup(std::function<void()> wakeup) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->set_wakeup(std::move(wakeup));
}
void ConnectionEngine::stop() noexcept { if (impl_) impl_->stop(); }
auto ConnectionEngine::generation() const noexcept -> GenerationId { return impl_ ? impl_->generation() : 0; }
auto ConnectionEngine::stats() const noexcept -> EngineStats { return impl_ ? impl_->stats() : EngineStats{}; }

} // namespace comicchat::net
