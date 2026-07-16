#include "comicchat/net/dcc_transfer_engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>

#include <uv.h>

namespace comicchat::net {
namespace {

auto handle_of(const DccCommand& command) noexcept -> DccTransferHandle {
    return std::visit([](const auto& value) { return value.handle; }, command);
}

auto parse_ipv4(const std::string_view text, sockaddr_in& address) noexcept -> bool {
    if (text.empty() || text.find('\0') != std::string_view::npos || text.size() >= 64) return false;
    const std::string value{text};
    return uv_ip4_addr(value.c_str(), 0, &address) == 0;
}

auto octets(const sockaddr_in& address) noexcept -> std::array<unsigned char, 4> {
    std::array<unsigned char, 4> output{};
    std::memcpy(output.data(), &address.sin_addr, output.size());
    return output;
}

auto is_unicast(const sockaddr_in& address) noexcept -> bool {
    const auto bytes = octets(address);
    return bytes[0] != 0 && bytes[0] < 224 && bytes[0] != 255;
}

auto numeric_address(const sockaddr_in& address) -> std::string {
    std::array<char, 64> output{};
    if (uv_ip4_name(&address, output.data(), output.size()) != 0) return {};
    return output.data();
}

auto same_address(const sockaddr_in& left, const sockaddr_in& right) noexcept -> bool {
    return std::memcmp(&left.sin_addr, &right.sin_addr, sizeof(left.sin_addr)) == 0;
}

auto valid_limits(const DccLimits& limits, const std::uint64_t file_size) noexcept -> bool {
    constexpr auto uv_buffer_limit = static_cast<std::size_t>(
        (std::numeric_limits<unsigned int>::max)());
    return file_size != 0 && file_size <= 0xffff'ffffULL && file_size <= limits.maximum_file_bytes &&
        limits.maximum_queued_send_bytes != 0 && limits.maximum_uncommitted_receive_bytes != 0 &&
        limits.receive_chunk_bytes != 0 &&
        limits.maximum_queued_send_bytes <= uv_buffer_limit &&
        limits.receive_chunk_bytes <= uv_buffer_limit &&
        limits.receive_chunk_bytes <= limits.maximum_uncommitted_receive_bytes &&
        limits.maximum_events >= 4 && limits.maximum_commands != 0;
}

auto valid_deadlines(const DccDeadlines& deadlines) noexcept -> bool {
    return deadlines.connect > std::chrono::milliseconds::zero() &&
        deadlines.accept > std::chrono::milliseconds::zero() &&
        deadlines.idle > std::chrono::milliseconds::zero();
}

} // namespace

auto dcc_legacy_ipv4_decimal(const std::string_view address)
    -> std::expected<std::uint32_t, DccError> {
    sockaddr_in parsed{};
    if (!parse_ipv4(address, parsed) || !is_unicast(parsed))
        return std::unexpected{DccError::invalid_address};
    const auto bytes = octets(parsed);
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
        (static_cast<std::uint32_t>(bytes[1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[2]) << 8U) |
        static_cast<std::uint32_t>(bytes[3]);
}

auto dcc_ipv4_scope(const std::string_view address)
    -> std::expected<DccAddressScope, DccError> {
    sockaddr_in parsed{};
    if (!parse_ipv4(address, parsed)) return std::unexpected{DccError::invalid_address};
    const auto bytes = octets(parsed);
    if (bytes[0] == 0) return DccAddressScope::unspecified;
    if (bytes[0] == 127) return DccAddressScope::loopback;
    if (bytes[0] == 169 && bytes[1] == 254) return DccAddressScope::link_local;
    if (bytes == std::array<unsigned char, 4>{255, 255, 255, 255})
        return DccAddressScope::limited_broadcast;
    if (bytes[0] >= 224 && bytes[0] <= 239) return DccAddressScope::multicast;
    if (bytes[0] >= 240) return DccAddressScope::reserved;
    if ((bytes[0] == 192 && bytes[1] == 0 && (bytes[2] == 0 || bytes[2] == 2)) ||
        (bytes[0] == 192 && bytes[1] == 88 && bytes[2] == 99) ||
        (bytes[0] == 198 && (bytes[1] == 18 || bytes[1] == 19 || bytes[1] == 51) &&
         (bytes[1] != 51 || bytes[2] == 100)) ||
        (bytes[0] == 203 && bytes[1] == 0 && bytes[2] == 113))
        return DccAddressScope::reserved;
    if (bytes[0] == 10 ||
        (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
        (bytes[0] == 192 && bytes[1] == 168) ||
        (bytes[0] == 100 && bytes[1] >= 64 && bytes[1] <= 127))
        return DccAddressScope::private_network;
    return DccAddressScope::public_unicast;
}

class DccTransferEngine::Impl final {
public:
    Impl() = default;
    ~Impl() { stop(); }

    auto start_listen(DccListenOptions options) -> std::expected<DccTransferHandle, DccError> {
        sockaddr_in bind{};
        if (!parse_ipv4(options.bind_address, bind)) return std::unexpected{DccError::invalid_address};
        const bool wildcard = octets(bind) == std::array<unsigned char, 4>{};
        if (!wildcard && !is_unicast(bind)) return std::unexpected{DccError::invalid_address};
        const auto advertise = options.advertise_address.value_or(options.bind_address);
        if (!dcc_legacy_ipv4_decimal(advertise)) return std::unexpected{DccError::invalid_address};
        if (wildcard && !options.advertise_address) return std::unexpected{DccError::invalid_address};
        if (options.expected_peer_address) {
            sockaddr_in expected{};
            if (!parse_ipv4(*options.expected_peer_address, expected) || !is_unicast(expected))
                return std::unexpected{DccError::invalid_address};
        }
        if (!valid_limits(options.limits, options.file_size) || !valid_deadlines(options.deadlines))
            return std::unexpected{DccError::invalid_options};
        return start_common(Direction::send, std::move(options), {});
    }

    auto start_connect(DccConnectOptions options) -> std::expected<DccTransferHandle, DccError> {
        sockaddr_in peer{};
        if (!parse_ipv4(options.peer_address, peer) || !is_unicast(peer) || options.port == 0)
            return std::unexpected{DccError::invalid_address};
        if (!valid_limits(options.limits, options.file_size) || !valid_deadlines(options.deadlines))
            return std::unexpected{DccError::invalid_options};
        return start_common(Direction::receive, {}, std::move(options));
    }

    auto post(DccCommand command) -> std::expected<void, DccError> {
        std::scoped_lock lock{mutex_};
        if (stopped_ || !accepting_commands_) return std::unexpected{DccError::not_running};
        if (handle_of(command) != handle_) return std::unexpected{DccError::stale_transfer};
        if (commands_.size() >= limits().maximum_commands) return std::unexpected{DccError::queue_full};
        std::size_t chunk_size{};
        bool chunk_final{};
        if (auto* chunk = std::get_if<DccQueueChunk>(&command)) {
            if (direction_ != Direction::send || chunk->bytes.empty() || final_posted_)
                return std::unexpected{DccError::protocol_error};
            if (chunk->bytes.size() > limits().maximum_queued_send_bytes ||
                queued_send_bytes_ > limits().maximum_queued_send_bytes - chunk->bytes.size())
                return std::unexpected{DccError::queue_full};
            if (accepted_send_bytes_ > file_size() ||
                chunk->bytes.size() > file_size() - accepted_send_bytes_)
                return std::unexpected{DccError::protocol_error};
            chunk_size = chunk->bytes.size();
            chunk_final = chunk->final;
            if (chunk->final) {
                if (accepted_send_bytes_ + chunk_size != file_size()) {
                    return std::unexpected{DccError::protocol_error};
                }
            }
        } else if (std::holds_alternative<DccCommitReceived>(command) && direction_ != Direction::receive) {
            return std::unexpected{DccError::protocol_error};
        } else if ((std::holds_alternative<DccAcceptPeer>(command) ||
                    std::holds_alternative<DccRejectPeer>(command)) && direction_ != Direction::send) {
            return std::unexpected{DccError::protocol_error};
        }
        try {
            commands_.push_back(std::move(command));
        } catch (const std::bad_alloc&) {
            return std::unexpected{DccError::allocation_failure};
        } catch (...) {
            return std::unexpected{DccError::allocation_failure};
        }
        if (chunk_size != 0) {
            accepted_send_bytes_ += chunk_size;
            queued_send_bytes_ += chunk_size;
            final_posted_ = final_posted_ || chunk_final;
        }
        if (loop_ready_.load(std::memory_order_acquire)) (void)uv_async_send(&wakeup_handle_);
        return {};
    }

    auto poll(const std::size_t maximum) -> std::vector<DccEvent> {
        std::scoped_lock lock{mutex_};
        std::vector<DccEvent> output;
        output.reserve(std::min(maximum, events_.size()));
        while (!events_.empty() && output.size() < maximum) {
            output.push_back(std::move(events_.front()));
            events_.pop_front();
        }
        return output;
    }

    void set_wakeup(std::function<void()> wakeup) {
        std::scoped_lock lock{mutex_};
        wakeup_ = std::move(wakeup);
    }

    void stop() noexcept {
        {
            std::scoped_lock lock{mutex_};
            stopped_ = true;
        }
        thread_.request_stop();
        if (loop_ready_.load(std::memory_order_acquire)) (void)uv_async_send(&wakeup_handle_);
        if (thread_.joinable() && std::this_thread::get_id() == thread_.get_id()) return;
        if (thread_.joinable()) {
            try {
                thread_.join();
            } catch (...) {
                // Detaching is unsafe because the worker captures Impl.
                // Self-thread stop is handled above, so fail closed.
                std::terminate();
            }
        }
        std::scoped_lock lock{mutex_};
        commands_.clear();
        queued_send_bytes_ = 0;
    }

    auto handle() const noexcept -> DccTransferHandle {
        std::scoped_lock lock{mutex_};
        return handle_;
    }

private:
    enum class Direction { send, receive };
    enum class Phase { starting, listening, offered, connecting, connected, terminal };
    enum class WriteKind { none, file, acknowledgement };

    struct Peer final {
        uv_tcp_t tcp{};
        Impl* owner{};
    };

    struct PendingChunk final {
        std::vector<std::byte> bytes;
        bool final{};
    };

    auto start_common(const Direction direction, DccListenOptions listen, DccConnectOptions connect)
        -> std::expected<DccTransferHandle, DccError> {
        std::unique_lock lock{mutex_};
        if (!stopped_) {
            if (accepting_commands_ || !thread_.joinable() ||
                thread_.get_id() == std::this_thread::get_id())
                return std::unexpected{DccError::already_running};
            lock.unlock();
            try {
                thread_.join();
            } catch (...) {
                return std::unexpected{DccError::already_running};
            }
            lock.lock();
            if (!stopped_) return std::unexpected{DccError::already_running};
        }
        direction_ = direction;
        listen_options_ = std::move(listen);
        connect_options_ = std::move(connect);
        ++handle_.generation;
        ++handle_.transfer;
        commands_.clear();
        events_.clear();
        queued_send_bytes_ = 0;
        accepted_send_bytes_ = 0;
        final_posted_ = false;
        listener_initialized_ = false;
        peer_ = nullptr;
        pending_peer_ = nullptr;
        pending_token_ = 0;
        next_peer_token_ = 0;
        pending_peer_address_.clear();
        phase_ = Phase::starting;
        send_queue_.clear();
        write_kind_ = WriteKind::none;
        write_bytes_.clear();
        write_final_ = false;
        final_written_ = false;
        transferred_ = 0;
        peer_committed_ = 0;
        ack_input_.clear();
        received_ = 0;
        committed_ = 0;
        pending_ack_ = 0;
        ack_inflight_ = 0;
        ack_sent_ = 0;
        read_paused_ = false;
        loop_initialized_ = false;
        wakeup_initialized_ = false;
        deadline_initialized_ = false;
        stopped_ = false;
        accepting_commands_ = true;
        terminal_ = false;
        thread_ = std::jthread{[this](const std::stop_token token) noexcept {
            network_thread_entry(token);
        }};
        return handle_;
    }

    auto limits() const noexcept -> const DccLimits& {
        return direction_ == Direction::send ? listen_options_.limits : connect_options_.limits;
    }

    auto deadlines() const noexcept -> const DccDeadlines& {
        return direction_ == Direction::send ? listen_options_.deadlines : connect_options_.deadlines;
    }

    auto file_size() const noexcept -> std::uint64_t {
        return direction_ == Direction::send ? listen_options_.file_size : connect_options_.file_size;
    }

    auto take_command() -> std::optional<DccCommand> {
        std::scoped_lock lock{mutex_};
        if (commands_.empty()) return std::nullopt;
        auto output = std::move(commands_.front());
        commands_.pop_front();
        return output;
    }

    auto publish(DccEventBody body) -> bool {
        std::function<void()> wakeup;
        {
            std::scoped_lock lock{mutex_};
            if (stopped_) return false;
            const auto is_terminal = [](const DccEventBody& value) {
                return std::holds_alternative<DccCompleted>(value) ||
                    std::holds_alternative<DccClosed>(value);
            };
            const bool terminal = is_terminal(body);
            const bool diagnostic = std::holds_alternative<DccDiagnostic>(body);

            auto replace_matching = [&](const auto predicate) {
                const auto found = std::find_if(events_.begin(), events_.end(), [&](const DccEvent& event) {
                    return predicate(event.body);
                });
                if (found == events_.end()) return false;
                found->body = std::move(body);
                return true;
            };

            bool replaced{};
            if (terminal) {
                // Completion and closure share one exactly-once reserved slot.
                // The first terminal outcome is authoritative.
                if (std::any_of(events_.begin(), events_.end(), [&](const DccEvent& event) {
                        return is_terminal(event.body);
                    }))
                    return true;
            } else if (diagnostic) {
                replaced = replace_matching([](const DccEventBody& event) {
                    return std::holds_alternative<DccDiagnostic>(event);
                });
            } else if (std::holds_alternative<DccProgress>(body)) {
                replaced = replace_matching([](const DccEventBody& event) {
                    return std::holds_alternative<DccProgress>(event);
                });
            } else if (std::holds_alternative<DccWritableCredit>(body)) {
                const auto incoming = std::get<DccWritableCredit>(body).bytes;
                const auto found = std::find_if(events_.begin(), events_.end(), [](const DccEvent& event) {
                    return std::holds_alternative<DccWritableCredit>(event.body);
                });
                if (found != events_.end()) {
                    auto& credit = std::get<DccWritableCredit>(found->body).bytes;
                    const auto cap = limits().maximum_queued_send_bytes;
                    credit = incoming > cap - std::min(credit, cap) ? cap : credit + incoming;
                    replaced = true;
                }
            }

            if (!replaced) {
                const auto capacity = limits().maximum_events;
                const auto ordinary_capacity = capacity - 2;
                if (!terminal && !diagnostic && events_.size() >= ordinary_capacity) return false;
                if (events_.size() >= capacity) return false;
                events_.push_back(DccEvent{handle_, std::move(body)});
            }
            wakeup = wakeup_;
        }
        if (wakeup) {
            try { wakeup(); } catch (...) {}
        }
        return true;
    }

    void publish_initialization_failure(const char* code, const char* message) noexcept {
        terminal_ = true;
        phase_ = Phase::terminal;
        set_accepting_commands(false);
        try { (void)publish(DccDiagnostic{code, message}); } catch (...) {}
        try { (void)publish(DccClosed{"transfer initialization failed"}); } catch (...) {}
    }

    void fail_noexcept(const char* code, const char* message) noexcept {
        try {
            fail(code, message);
        } catch (...) {
            terminal_ = true;
            phase_ = Phase::terminal;
            set_accepting_commands(false);
        }
        thread_.request_stop();
        close_network();
    }

    template <typename Function>
    static void callback_boundary(Impl* self, Function&& function) noexcept {
        try {
            function();
        } catch (...) {
            self->fail_noexcept("callback-exception", "DCC callback failed safely");
        }
    }

    void network_thread_entry(const std::stop_token token) noexcept {
        try {
            network_loop(token);
        } catch (...) {
            fail_noexcept("network-exception", "DCC network thread failed safely");
        }
        cleanup_network_loop();
        loop_ready_.store(false, std::memory_order_release);
        {
            std::scoped_lock lock{mutex_};
            stopped_ = true;
            accepting_commands_ = false;
            commands_.clear();
            queued_send_bytes_ = 0;
        }
    }

    void network_loop(const std::stop_token token) {
        if (uv_loop_init(&loop_) != 0) {
            publish_initialization_failure("loop-init", "DCC event loop initialization failed");
            return;
        }
        loop_initialized_ = true;
        wakeup_handle_.data = this;
        deadline_timer_.data = this;
        if (uv_async_init(&loop_, &wakeup_handle_, [](uv_async_t*) {}) != 0) {
            publish_initialization_failure("handle-init", "DCC event handles could not initialize");
            return;
        }
        wakeup_initialized_ = true;
        if (uv_timer_init(&loop_, &deadline_timer_) != 0) {
            publish_initialization_failure("handle-init", "DCC event handles could not initialize");
            return;
        }
        deadline_initialized_ = true;
        loop_ready_.store(true, std::memory_order_release);
        if (direction_ == Direction::send) begin_listen();
        else begin_connect();
        while (!token.stop_requested()) {
            while (auto command = take_command()) process(std::move(*command));
            (void)uv_run(&loop_, UV_RUN_ONCE);
        }
        terminal_ = true;
        close_network();
    }

    void cleanup_network_loop() noexcept {
        if (!loop_initialized_) return;
        close_network();
        if (wakeup_initialized_ && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&wakeup_handle_)))
            uv_close(reinterpret_cast<uv_handle_t*>(&wakeup_handle_), nullptr);
        if (deadline_initialized_ && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&deadline_timer_)))
            uv_close(reinterpret_cast<uv_handle_t*>(&deadline_timer_), nullptr);
        while (uv_run(&loop_, UV_RUN_DEFAULT) != 0) {}
        (void)uv_loop_close(&loop_);
        loop_initialized_ = false;
        wakeup_initialized_ = false;
        deadline_initialized_ = false;
    }

    void begin_listen() {
        sockaddr_in address{};
        if (!parse_ipv4(listen_options_.bind_address, address)) {
            fail("bind-address", "DCC bind address is invalid");
            return;
        }
        address.sin_port = 0;
        if (uv_ip4_addr(listen_options_.bind_address.c_str(), listen_options_.port, &address) != 0 ||
            uv_tcp_init(&loop_, &listener_) != 0) {
            fail("listen-init", "DCC listener could not initialize");
            return;
        }
        listener_initialized_ = true;
        listener_.data = this;
        if (uv_tcp_bind(&listener_, reinterpret_cast<const sockaddr*>(&address), 0) != 0 ||
            uv_listen(reinterpret_cast<uv_stream_t*>(&listener_), 1, listener_callback) != 0) {
            fail("listen", "DCC listener could not bind or listen");
            return;
        }
        sockaddr_storage bound{};
        int size = sizeof(bound);
        if (uv_tcp_getsockname(&listener_, reinterpret_cast<sockaddr*>(&bound), &size) != 0 ||
            bound.ss_family != AF_INET) {
            fail("listen-address", "DCC listener address could not be queried");
            return;
        }
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&bound);
        const auto advertise = listen_options_.advertise_address.value_or(listen_options_.bind_address);
        const auto decimal = dcc_legacy_ipv4_decimal(advertise);
        if (!decimal) {
            fail("advertise-address", "DCC advertised address is invalid");
            return;
        }
        phase_ = Phase::listening;
        arm_deadline(deadlines().accept);
        std::array<unsigned char, 2> port_bytes{};
        std::memcpy(port_bytes.data(), &ipv4->sin_port, port_bytes.size());
        const auto port = static_cast<std::uint16_t>(
            (static_cast<unsigned int>(port_bytes[0]) << 8U) | static_cast<unsigned int>(port_bytes[1]));
        if (!publish(DccListening{numeric_address(*ipv4), advertise, port,
                                  *decimal}))
            fail("event-backpressure", "DCC event queue is full");
    }

    static void listener_callback(uv_stream_t* stream, const int status) {
        auto* self = static_cast<Impl*>(stream->data);
        callback_boundary(self, [&] {
            if (status < 0) self->fail("accept", "DCC listener reported an accept error");
            else self->offer_peer();
        });
    }

    void offer_peer() {
        auto* candidate = new (std::nothrow) Peer;
        if (candidate == nullptr || uv_tcp_init(&loop_, &candidate->tcp) != 0) {
            delete candidate;
            fail("accept-memory", "DCC peer could not be allocated");
            return;
        }
        candidate->owner = this;
        candidate->tcp.data = candidate;
        if (uv_accept(reinterpret_cast<uv_stream_t*>(&listener_),
                      reinterpret_cast<uv_stream_t*>(&candidate->tcp)) != 0) {
            close_peer(candidate);
            return;
        }
        sockaddr_storage remote{};
        int size = sizeof(remote);
        if (uv_tcp_getpeername(&candidate->tcp, reinterpret_cast<sockaddr*>(&remote), &size) != 0 ||
            remote.ss_family != AF_INET || !is_unicast(*reinterpret_cast<const sockaddr_in*>(&remote))) {
            close_peer(candidate);
            (void)publish(DccDiagnostic{"peer-address", "Rejected a DCC peer with an invalid address"});
            return;
        }
        const auto* remote4 = reinterpret_cast<const sockaddr_in*>(&remote);
        if (listen_options_.expected_peer_address) {
            sockaddr_in expected{};
            (void)parse_ipv4(*listen_options_.expected_peer_address, expected);
            if (!same_address(expected, *remote4)) {
                close_peer(candidate);
                (void)publish(DccDiagnostic{"unexpected-peer", "Rejected a DCC peer that did not match the offer"});
                return;
            }
        }
        if (pending_peer_ != nullptr || peer_ != nullptr) {
            close_peer(candidate);
            return;
        }
        pending_peer_ = candidate;
        pending_peer_address_ = numeric_address(*remote4);
        pending_token_ = ++next_peer_token_;
        phase_ = Phase::offered;
        if (!publish(DccPeerOffered{pending_token_, pending_peer_address_}))
            fail("event-backpressure", "DCC event queue is full");
    }

    void begin_connect() {
        sockaddr_in address{};
        (void)uv_ip4_addr(connect_options_.peer_address.c_str(), connect_options_.port, &address);
        peer_ = new (std::nothrow) Peer;
        if (peer_ == nullptr || uv_tcp_init(&loop_, &peer_->tcp) != 0) {
            delete peer_;
            peer_ = nullptr;
            fail("connect-memory", "DCC peer could not be allocated");
            return;
        }
        peer_->owner = this;
        peer_->tcp.data = peer_;
        connect_request_.data = this;
        phase_ = Phase::connecting;
        arm_deadline(deadlines().connect);
        if (uv_tcp_connect(&connect_request_, &peer_->tcp, reinterpret_cast<const sockaddr*>(&address),
                           connect_callback) != 0)
            fail("connect", "DCC connection could not start");
    }

    static void connect_callback(uv_connect_t* request, const int status) {
        auto* self = static_cast<Impl*>(request->data);
        callback_boundary(self, [&] {
            if (status < 0) self->fail("connect", "DCC connection failed");
            else self->connected(self->connect_options_.peer_address);
        });
    }

    void connected(std::string address) {
        phase_ = Phase::connected;
        arm_deadline(deadlines().idle);
        if (!publish(DccPeerConnected{std::move(address)}) ||
            !publish(DccWritableCredit{limits().maximum_queued_send_bytes})) {
            fail("event-backpressure", "DCC event queue is full");
            return;
        }
        start_reading();
        if (direction_ == Direction::send) pump_send();
    }

    void process(DccCommand command) {
        if (auto* chunk = std::get_if<DccQueueChunk>(&command)) {
            send_queue_.push_back(PendingChunk{std::move(chunk->bytes), chunk->final});
            pump_send();
            return;
        }
        if (const auto* commit = std::get_if<DccCommitReceived>(&command)) {
            if (commit->through_offset < committed_ || commit->through_offset > received_) {
                fail("commit-range", "DCC receive commit was outside the delivered range");
                return;
            }
            committed_ = commit->through_offset;
            pending_ack_ = committed_;
            pump_ack();
            if (read_paused_ && received_ - committed_ < limits().maximum_uncommitted_receive_bytes)
                start_reading();
            return;
        }
        if (const auto* accept = std::get_if<DccAcceptPeer>(&command)) {
            if (pending_peer_ == nullptr || accept->peer != pending_token_) {
                fail("peer-token", "DCC peer approval token was stale");
                return;
            }
            peer_ = pending_peer_;
            pending_peer_ = nullptr;
            const auto address = std::move(pending_peer_address_);
            close_listener();
            connected(address);
            return;
        }
        if (const auto* reject = std::get_if<DccRejectPeer>(&command)) {
            if (pending_peer_ == nullptr || reject->peer != pending_token_) {
                fail("peer-token", "DCC peer rejection token was stale");
                return;
            }
            close_peer(pending_peer_);
            pending_peer_address_.clear();
            phase_ = Phase::listening;
            arm_deadline(deadlines().accept);
            return;
        }
        const auto& cancel = std::get<DccCancel>(command);
        finish_closed(cancel.reason.empty() ? "cancelled" : cancel.reason);
    }

    static void allocate_read(uv_handle_t* handle, const std::size_t suggested, uv_buf_t* buffer) {
        auto* peer = static_cast<Peer*>(handle->data);
        auto* self = peer->owner;
        if (self->direction_ == Direction::send) {
            const auto amount = std::max<std::size_t>(1, std::min({
                suggested, self->limits().receive_chunk_bytes,
                static_cast<std::size_t>(64U * 1024U)}));
            buffer->base = new (std::nothrow) char[amount];
            buffer->len = buffer->base == nullptr ? 0 : amount;
            return;
        }
        const auto uncommitted = self->received_ - self->committed_;
        const auto receive_credit = self->limits().maximum_uncommitted_receive_bytes -
            static_cast<std::size_t>(uncommitted);
        const auto file_credit = static_cast<std::size_t>(std::min<std::uint64_t>(
            self->file_size() - self->received_, std::numeric_limits<std::size_t>::max()));
        const auto amount = std::min({suggested,
            self->limits().receive_chunk_bytes, receive_credit, file_credit,
            static_cast<std::size_t>(64U * 1024U)});
        if (amount == 0) {
            (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(handle));
            self->read_paused_ = true;
            *buffer = uv_buf_init(nullptr, 0);
            return;
        }
        buffer->base = new (std::nothrow) char[amount];
        buffer->len = buffer->base == nullptr ? 0 : amount;
    }

    static void read_callback(uv_stream_t* stream, const std::ptrdiff_t count, const uv_buf_t* buffer) {
        auto* peer = static_cast<Peer*>(stream->data);
        auto* self = peer->owner;
        std::unique_ptr<char[]> storage{buffer->base};
        callback_boundary(self, [&] {
            if (self->terminal_) return;
            if (count == UV_ENOBUFS && self->read_paused_) return;
            if (count < 0) {
                self->fail(count == UV_EOF ? "early-eof" : "read",
                           count == UV_EOF ? "DCC peer closed before completion" : "DCC read failed");
                return;
            }
            if (count == 0) return;
            self->arm_deadline(self->deadlines().idle);
            const auto size = static_cast<std::size_t>(count);
            if (self->direction_ == Direction::send) self->read_acknowledgements(buffer->base, size);
            else self->read_file_bytes(buffer->base, size);
        });
    }

    void start_reading() {
        if (peer_ == nullptr || terminal_) return;
        if (direction_ == Direction::receive &&
            (received_ == file_size() ||
             received_ - committed_ >= limits().maximum_uncommitted_receive_bytes)) {
            read_paused_ = true;
            return;
        }
        if (uv_read_start(reinterpret_cast<uv_stream_t*>(&peer_->tcp), allocate_read, read_callback) != 0) {
            fail("read-start", "DCC reading could not start");
            return;
        }
        read_paused_ = false;
    }

    void read_acknowledgements(const char* data, const std::size_t size) {
        for (std::size_t index = 0; index < size; ++index) {
            ack_input_.push_back(std::byte{static_cast<unsigned char>(data[index])});
            if (ack_input_.size() != 4) continue;
            const auto value = (static_cast<std::uint32_t>(std::to_integer<unsigned char>(ack_input_[0])) << 24U) |
                (static_cast<std::uint32_t>(std::to_integer<unsigned char>(ack_input_[1])) << 16U) |
                (static_cast<std::uint32_t>(std::to_integer<unsigned char>(ack_input_[2])) << 8U) |
                static_cast<std::uint32_t>(std::to_integer<unsigned char>(ack_input_[3]));
            ack_input_.clear();
            if (value < peer_committed_ || value > transferred_) {
                fail("ack-range", "DCC peer acknowledgement exceeded transmitted bytes");
                return;
            }
            if (value == peer_committed_) continue;
            peer_committed_ = value;
            if (!publish(DccProgress{transferred_, peer_committed_, file_size()})) {
                fail("event-backpressure", "DCC event queue is full");
                return;
            }
            if (final_written_ && peer_committed_ == file_size()) {
                complete();
                return;
            }
        }
    }

    void read_file_bytes(const char* data, const std::size_t size) {
        if (size > file_size() - received_ ||
            size > limits().maximum_uncommitted_receive_bytes - static_cast<std::size_t>(received_ - committed_)) {
            fail("receive-bounds", "DCC peer exceeded the announced or uncommitted receive bound");
            return;
        }
        auto bytes = std::make_shared<std::vector<std::byte>>(size);
        std::memcpy(bytes->data(), data, size);
        const auto offset = received_;
        received_ += size;
        if (!publish(DccChunkReceived{offset, std::move(bytes)})) {
            fail("event-backpressure", "DCC event queue is full");
            return;
        }
        bool event_capacity_exhausted{};
        {
            std::scoped_lock lock{mutex_};
            event_capacity_exhausted = events_.size() + 2 >= limits().maximum_events;
        }
        if (received_ == file_size() || received_ - committed_ >= limits().maximum_uncommitted_receive_bytes ||
            event_capacity_exhausted) {
            (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(&peer_->tcp));
            read_paused_ = true;
        }
    }

    void pump_send() {
        if (direction_ != Direction::send || phase_ != Phase::connected || write_kind_ != WriteKind::none ||
            send_queue_.empty() || terminal_) return;
        auto chunk = std::move(send_queue_.front());
        send_queue_.pop_front();
        write_bytes_ = std::move(chunk.bytes);
        write_final_ = chunk.final;
        write_request_.data = this;
        uv_buf_t buffer = uv_buf_init(reinterpret_cast<char*>(write_bytes_.data()),
                                      static_cast<unsigned int>(write_bytes_.size()));
        write_kind_ = WriteKind::file;
        if (uv_write(&write_request_, reinterpret_cast<uv_stream_t*>(&peer_->tcp), &buffer, 1, write_callback) != 0)
            fail("write", "DCC file write could not start");
    }

    void pump_ack() {
        if (direction_ != Direction::receive || phase_ != Phase::connected ||
            write_kind_ != WriteKind::none || pending_ack_ <= ack_sent_ || terminal_) return;
        ack_inflight_ = pending_ack_;
        ack_bytes_ = {
            std::byte{static_cast<unsigned char>((ack_inflight_ >> 24U) & 0xffU)},
            std::byte{static_cast<unsigned char>((ack_inflight_ >> 16U) & 0xffU)},
            std::byte{static_cast<unsigned char>((ack_inflight_ >> 8U) & 0xffU)},
            std::byte{static_cast<unsigned char>(ack_inflight_ & 0xffU)},
        };
        write_request_.data = this;
        uv_buf_t buffer = uv_buf_init(reinterpret_cast<char*>(ack_bytes_.data()),
                                      static_cast<unsigned int>(ack_bytes_.size()));
        write_kind_ = WriteKind::acknowledgement;
        if (uv_write(&write_request_, reinterpret_cast<uv_stream_t*>(&peer_->tcp), &buffer, 1, write_callback) != 0)
            fail("ack-write", "DCC acknowledgement could not start");
    }

    static void write_callback(uv_write_t* request, const int status) {
        auto* self = static_cast<Impl*>(request->data);
        callback_boundary(self, [&] {
            if (self->terminal_) return;
            if (status < 0) {
                self->fail("write", "DCC write failed");
                return;
            }
            self->arm_deadline(self->deadlines().idle);
            if (self->write_kind_ == WriteKind::file) {
                const auto count = self->write_bytes_.size();
                self->transferred_ += count;
                std::size_t credit{};
                {
                    std::scoped_lock lock{self->mutex_};
                    self->queued_send_bytes_ = count > self->queued_send_bytes_ ? 0 : self->queued_send_bytes_ - count;
                    credit = count;
                }
                self->write_bytes_.clear();
                self->final_written_ = self->final_written_ || self->write_final_;
                self->write_kind_ = WriteKind::none;
                if (!self->publish(DccProgress{self->transferred_, self->peer_committed_, self->file_size()}) ||
                    !self->publish(DccWritableCredit{credit})) {
                    self->fail("event-backpressure", "DCC event queue is full");
                    return;
                }
                self->pump_send();
            } else {
                self->ack_sent_ = self->ack_inflight_;
                self->write_kind_ = WriteKind::none;
                if (!self->publish(DccProgress{self->received_, self->ack_sent_, self->file_size()})) {
                    self->fail("event-backpressure", "DCC event queue is full");
                    return;
                }
                if (self->received_ == self->file_size() && self->ack_sent_ == self->file_size()) self->complete();
                else self->pump_ack();
            }
        });
    }

    void arm_deadline(const std::chrono::milliseconds duration) {
        (void)uv_timer_stop(&deadline_timer_);
        (void)uv_timer_start(&deadline_timer_, [](uv_timer_t* timer) {
            auto* self = static_cast<Impl*>(timer->data);
            callback_boundary(self, [&] {
                if (self->phase_ == Phase::listening || self->phase_ == Phase::offered)
                    self->fail("accept-timeout", "DCC accept deadline expired");
                else if (self->phase_ == Phase::connecting)
                    self->fail("connect-timeout", "DCC connect deadline expired");
                else if (self->phase_ == Phase::connected)
                    self->fail("idle-timeout", "DCC idle deadline expired");
            });
        }, static_cast<std::uint64_t>(duration.count()), 0);
    }

    void complete() {
        if (terminal_) return;
        terminal_ = true;
        phase_ = Phase::terminal;
        (void)uv_timer_stop(&deadline_timer_);
        set_accepting_commands(false);
        (void)publish(DccCompleted{file_size()});
        thread_.request_stop();
        close_network();
    }

    void fail(std::string code, std::string message) {
        if (terminal_) return;
        terminal_ = true;
        phase_ = Phase::terminal;
        (void)uv_timer_stop(&deadline_timer_);
        set_accepting_commands(false);
        (void)publish(DccDiagnostic{std::move(code), std::move(message)});
        (void)publish(DccClosed{"transfer failed"});
        thread_.request_stop();
        close_network();
    }

    void finish_closed(std::string reason) {
        if (terminal_) return;
        terminal_ = true;
        phase_ = Phase::terminal;
        (void)uv_timer_stop(&deadline_timer_);
        set_accepting_commands(false);
        (void)publish(DccClosed{std::move(reason)});
        thread_.request_stop();
        close_network();
    }

    static void delete_peer(uv_handle_t* handle) { delete static_cast<Peer*>(handle->data); }

    void close_peer(Peer*& peer) noexcept {
        if (peer == nullptr) return;
        auto* closing = peer;
        peer = nullptr;
        (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(&closing->tcp));
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&closing->tcp)))
            uv_close(reinterpret_cast<uv_handle_t*>(&closing->tcp), delete_peer);
    }

    void close_listener() noexcept {
        if (listener_initialized_ && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&listener_)))
            uv_close(reinterpret_cast<uv_handle_t*>(&listener_), nullptr);
    }

    void close_network() noexcept {
        close_listener();
        close_peer(pending_peer_);
        close_peer(peer_);
    }

    void set_accepting_commands(const bool accepting) noexcept {
        std::scoped_lock lock{mutex_};
        accepting_commands_ = accepting;
    }

    mutable std::mutex mutex_;
    std::deque<DccCommand> commands_;
    std::deque<DccEvent> events_;
    std::function<void()> wakeup_;
    DccListenOptions listen_options_;
    DccConnectOptions connect_options_;
    DccTransferHandle handle_;
    Direction direction_{Direction::send};
    std::jthread thread_;
    bool stopped_{true};
    bool accepting_commands_{};
    std::size_t queued_send_bytes_{};
    std::uint64_t accepted_send_bytes_{};
    bool final_posted_{};

    uv_loop_t loop_{};
    uv_async_t wakeup_handle_{};
    uv_timer_t deadline_timer_{};
    uv_tcp_t listener_{};
    uv_connect_t connect_request_{};
    uv_write_t write_request_{};
    std::atomic_bool loop_ready_{};
    bool loop_initialized_{};
    bool wakeup_initialized_{};
    bool deadline_initialized_{};
    bool listener_initialized_{};
    Peer* peer_{};
    Peer* pending_peer_{};
    DccPeerToken pending_token_{};
    DccPeerToken next_peer_token_{};
    std::string pending_peer_address_;
    Phase phase_{Phase::starting};
    bool terminal_{};
    bool read_paused_{};

    std::deque<PendingChunk> send_queue_;
    WriteKind write_kind_{WriteKind::none};
    std::vector<std::byte> write_bytes_;
    bool write_final_{};
    bool final_written_{};
    std::uint64_t transferred_{};
    std::uint64_t peer_committed_{};
    std::vector<std::byte> ack_input_;

    std::uint64_t received_{};
    std::uint64_t committed_{};
    std::uint64_t pending_ack_{};
    std::uint64_t ack_inflight_{};
    std::uint64_t ack_sent_{};
    std::array<std::byte, 4> ack_bytes_{};
};

DccTransferEngine::DccTransferEngine() : impl_{std::make_unique<Impl>()} {}
DccTransferEngine::~DccTransferEngine() = default;
DccTransferEngine::DccTransferEngine(DccTransferEngine&&) noexcept = default;
auto DccTransferEngine::operator=(DccTransferEngine&&) noexcept -> DccTransferEngine& = default;
auto DccTransferEngine::start_listen(DccListenOptions options)
    -> std::expected<DccTransferHandle, DccError> { return impl_->start_listen(std::move(options)); }
auto DccTransferEngine::start_connect(DccConnectOptions options)
    -> std::expected<DccTransferHandle, DccError> { return impl_->start_connect(std::move(options)); }
auto DccTransferEngine::post(DccCommand command) -> std::expected<void, DccError> {
    return impl_->post(std::move(command));
}
auto DccTransferEngine::poll_events(const std::size_t maximum) -> std::vector<DccEvent> {
    return impl_->poll(maximum);
}
void DccTransferEngine::set_wakeup(std::function<void()> wakeup) { impl_->set_wakeup(std::move(wakeup)); }
void DccTransferEngine::stop() noexcept { impl_->stop(); }
auto DccTransferEngine::handle() const noexcept -> DccTransferHandle { return impl_->handle(); }

} // namespace comicchat::net
