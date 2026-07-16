#pragma once

#include "comicchat/cpp26.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace comicchat::net {

using GenerationId = std::uint64_t;
using SendId = std::uint64_t;

enum class Security { tls, plaintext };
enum class ProxyKind { none, socks5, http_connect };
enum class Priority { control, authentication, pong, chat, bulk };
enum class State { stopped, resolving, connecting, proxy_handshake, tls_handshake, connected, reconnect_wait };

struct Endpoint final { std::string host; std::uint16_t port{6697}; };
struct Proxy final {
    ProxyKind kind{ProxyKind::none};
    std::string host;
    std::uint16_t port{};
    std::optional<std::string> username;
    std::optional<std::string> password;
};
struct Limits final {
    std::size_t receive_bytes{256U * 1024U};
    std::size_t transmit_bytes{256U * 1024U};
    std::size_t queued_commands{1024};
};
struct Deadlines final {
    std::chrono::milliseconds connect{10'000};
    std::chrono::milliseconds handshake{10'000};
    std::chrono::milliseconds idle{120'000};
    std::chrono::milliseconds ping{30'000};
};
struct ConnectionOptions final {
    Endpoint endpoint;
    Security security{Security::tls};
    std::optional<std::string> ca_file;
    std::string server_name;
    Proxy proxy;
    Limits limits;
    Deadlines deadlines;
    bool enable_session_resumption{true};
    // Supplying this is useful for deterministic retry tests. Production
    // callers should leave it empty so each engine uses OS entropy.
    std::optional<std::uint64_t> reconnect_jitter_seed;
};

struct Connect final { GenerationId generation{}; ConnectionOptions options; };
struct Disconnect final { GenerationId generation{}; std::string reason; };
struct Send final {
    GenerationId generation{};
    SendId id{};
    Priority priority{Priority::chat};
    std::vector<std::byte> bytes;
    bool sensitive{};
};
using Command = std::variant<Connect, Disconnect, Send>;

struct StateChanged final { State state{}; };
struct Connected final {
    std::string peer;
    std::string local_address;
    bool tls{};
    bool resumed{};
};
struct BytesReceived final { std::shared_ptr<const std::vector<std::byte>> bytes; };
struct SendComplete final { SendId id{}; };
struct Closed final { std::string reason; std::chrono::milliseconds retry_after{}; };
struct Diagnostic final { std::string code; std::string message; };
using EventBody = std::variant<StateChanged, Connected, BytesReceived, SendComplete, Closed, Diagnostic>;
struct Event final { GenerationId generation{}; EventBody body; };

struct EngineStats final {
    std::uint64_t loop_iterations{};
    std::uint64_t command_wakeups{};
    std::uint64_t rejected_sensitive_bytes_wiped{};
};

enum class EngineError { already_running, not_running, stale_generation, queue_full, invalid_options, crypto_unavailable };

class ConnectionEngine final {
public:
    ConnectionEngine();
    ~ConnectionEngine();
    ConnectionEngine(const ConnectionEngine&) = delete;
    auto operator=(const ConnectionEngine&) -> ConnectionEngine& = delete;
    ConnectionEngine(ConnectionEngine&&) noexcept;
    auto operator=(ConnectionEngine&&) noexcept -> ConnectionEngine&;

    [[nodiscard]] auto start(ConnectionOptions options) -> std::expected<GenerationId, EngineError>;
    [[nodiscard]] auto post(Command command) -> std::expected<void, EngineError>;
    [[nodiscard]] auto poll_events(std::size_t maximum = 128) -> std::vector<Event>;
    void set_wakeup(std::function<void()> wakeup);
    void stop() noexcept;
    [[nodiscard]] auto generation() const noexcept -> GenerationId;
    [[nodiscard]] auto stats() const noexcept -> EngineStats;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace comicchat::net
