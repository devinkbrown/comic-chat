#pragma once

#include "comicchat/cpp26.hpp"
#include "comicchat/net/connection_engine.hpp"
#include "comicchat/net/ircv3.hpp"
#include "comicchat/net/sts_policy_store.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace comicchat::net {

enum class StsSessionError : std::uint8_t {
    not_loaded,
    unhealthy,
    already_active,
    no_active_connection,
    stale_generation,
    invalid_transport_security,
    invalid_protocol_update,
    transport_start_failure,
    callback_failure,
    store_failure,
};

struct StsSessionFailure final {
    StsSessionError code{};
    std::optional<StsStoreError> store_error;
    std::optional<EngineError> engine_error;
};

struct StsTransportPlan final {
    Endpoint endpoint;
    Security security{Security::tls};
};

struct StsSessionStart final {
    GenerationId generation{};
    bool enforced{};
    StsTransportPlan transport;
};

enum class StsProtocolDisposition : std::uint8_t {
    continued,
    reconnected,
};

// UI/session-thread owner for the causal boundary between durable STS state,
// ConnectionEngine, and IRCv3 output. It intentionally has no MFC dependency.
// A single instance owns one requested-hostname session and may span the
// ConnectionEngine's same-generation internal retries.
class StsSessionPolicy final {
public:
    using StartTransport = std::function<
        std::expected<GenerationId, EngineError>(ConnectionOptions)>;
    using ReconnectSecure = std::function<bool(std::uint16_t)>;
    using ContinueProtocolOutput = std::function<bool()>;

    explicit StsSessionPolicy(std::filesystem::path policy_file);

    [[nodiscard]] auto load(StsTimePoint now) -> std::expected<void, StsSessionFailure>;

    // The durable plan is calculated before start_transport is invoked. An
    // active policy therefore replaces plaintext/port before every external
    // ConnectionEngine::start call. The callback must consume the exact options
    // it receives; bypassing this owner is a session-adapter defect.
    [[nodiscard]] auto start(
        ConnectionOptions requested,
        StsTimePoint now,
        const StartTransport& start_transport)
        -> std::expected<StsSessionStart, StsSessionFailure>;

    // Must be called only for the matching Connected event. tls_verified is
    // true only after certificate and hostname verification succeeds.
    [[nodiscard]] auto connected(GenerationId generation, bool tls_verified)
        -> std::expected<void, StsSessionFailure>;

    // This is the only path from ProcessResult::sts_update to protocol output.
    // Upgrade invokes reconnect_secure and never invokes continue_output.
    // Persist/Remove commits durable state before continue_output is released.
    [[nodiscard]] auto route_protocol_update(
        const std::optional<comic_chat::ircv3::StsPolicyUpdate>& update,
        GenerationId generation,
        StsTimePoint now,
        const ReconnectSecure& reconnect_secure,
        const ContinueProtocolOutput& continue_output)
        -> std::expected<StsProtocolDisposition, StsSessionFailure>;

    // Reschedules the last successfully persisted duration for a verified
    // transport disconnect and always clears that transport's receipt. Set
    // retain_for_retry only for ConnectionEngine's same-generation retry;
    // replacement, explicit close, and terminal failure end the active plan.
    [[nodiscard]] auto transport_disconnected(
        GenerationId generation,
        bool retain_for_retry,
        StsTimePoint now) -> std::expected<void, StsSessionFailure>;

    [[nodiscard]] auto healthy() const noexcept -> bool { return healthy_; }
    [[nodiscard]] auto ready() const noexcept -> bool { return healthy_ && store_.loaded(); }
    [[nodiscard]] auto active_generation() const noexcept -> GenerationId;
    [[nodiscard]] auto active_options() const noexcept -> std::optional<StsTransportPlan>;
    [[nodiscard]] auto has_persistence_receipt() const noexcept -> bool;

private:
    struct ActiveConnection final {
        GenerationId generation{};
        std::string requested_hostname;
        StsTransportPlan transport;
        bool connected{};
        bool tls_verified{};
        std::optional<StsPolicyReceipt> receipt;
    };

    [[nodiscard]] static auto Failure(StsSessionError code) -> StsSessionFailure;
    [[nodiscard]] auto StoreFailure(StsStoreError error) -> StsSessionFailure;
    [[nodiscard]] auto Current(GenerationId generation)
        -> std::expected<ActiveConnection*, StsSessionFailure>;

    StsPolicyStore store_;
    std::optional<ActiveConnection> active_;
    bool healthy_{true};
};

} // namespace comicchat::net
