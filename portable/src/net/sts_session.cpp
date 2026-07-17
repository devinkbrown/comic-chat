#include "comicchat/net/sts_session.hpp"

#include <exception>
#include <optional>
#include <string_view>
#include <utility>

namespace comicchat::net {

StsSessionPolicy::StsSessionPolicy(std::filesystem::path policy_file)
    : store_(std::move(policy_file)) {}

auto StsSessionPolicy::Failure(const StsSessionError code) -> StsSessionFailure {
    return StsSessionFailure{code, std::nullopt, std::nullopt};
}

auto StsSessionPolicy::StoreFailure(const StsStoreError error) -> StsSessionFailure {
    healthy_ = false;
    return StsSessionFailure{StsSessionError::store_failure, error, std::nullopt};
}

auto StsSessionPolicy::Current(const GenerationId generation)
    -> std::expected<ActiveConnection*, StsSessionFailure> {
    if (!active_) return std::unexpected(Failure(StsSessionError::no_active_connection));
    if (active_->generation != generation) {
        return std::unexpected(Failure(StsSessionError::stale_generation));
    }
    return &*active_;
}

auto StsSessionPolicy::load(const StsTimePoint now) -> std::expected<void, StsSessionFailure> {
    if (active_) return std::unexpected(Failure(StsSessionError::already_active));
    const auto loaded = store_.load(now);
    if (!loaded) return std::unexpected(StoreFailure(loaded.error()));
    healthy_ = true;
    return {};
}

auto StsSessionPolicy::start(
    ConnectionOptions requested,
    const StsTimePoint now,
    const StartTransport& start_transport)
    -> std::expected<StsSessionStart, StsSessionFailure> {
    if (!store_.loaded()) return std::unexpected(Failure(StsSessionError::not_loaded));
    if (!healthy_) return std::unexpected(Failure(StsSessionError::unhealthy));
    if (active_) return std::unexpected(Failure(StsSessionError::already_active));
    if (!start_transport) return std::unexpected(Failure(StsSessionError::transport_start_failure));

    auto planned = store_.plan(std::move(requested), now);
    if (!planned) return std::unexpected(StoreFailure(planned.error()));
    const auto requested_hostname = planned->options.server_name.empty()
                                        ? planned->options.endpoint.host
                                        : planned->options.server_name;
    const StsTransportPlan transport{planned->options.endpoint, planned->options.security};
    const bool enforced = planned->enforced;

    std::expected<GenerationId, EngineError> started =
        std::unexpected(EngineError::invalid_options);
    try {
        started = start_transport(std::move(planned->options));
    } catch (...) {
        return std::unexpected(Failure(StsSessionError::transport_start_failure));
    }
    if (!started || *started == 0) {
        auto failure = Failure(StsSessionError::transport_start_failure);
        if (!started) failure.engine_error = started.error();
        return std::unexpected(std::move(failure));
    }

    active_.emplace(ActiveConnection{
        *started,
        requested_hostname,
        transport,
        false,
        false,
        std::nullopt,
    });
    return StsSessionStart{*started, enforced, transport};
}

auto StsSessionPolicy::connected(const GenerationId generation, const bool tls_verified)
    -> std::expected<void, StsSessionFailure> {
    const auto current = Current(generation);
    if (!current) return std::unexpected(current.error());
    auto& connection = **current;
    const bool planned_tls = connection.transport.security == Security::tls;
    if (connection.connected || planned_tls != tls_verified) {
        return std::unexpected(Failure(StsSessionError::invalid_transport_security));
    }
    connection.connected = true;
    connection.tls_verified = tls_verified;
    return {};
}

auto StsSessionPolicy::route_protocol_update(
    const std::optional<comic_chat::ircv3::StsPolicyUpdate>& update,
    const GenerationId generation,
    const StsTimePoint now,
    const ReconnectSecure& reconnect_secure,
    const ContinueProtocolOutput& continue_output)
    -> std::expected<StsProtocolDisposition, StsSessionFailure> {
    if (!healthy_) return std::unexpected(Failure(StsSessionError::unhealthy));
    const auto current = Current(generation);
    if (!current) return std::unexpected(current.error());
    auto& connection = **current;
    if (!connection.connected) {
        return std::unexpected(Failure(StsSessionError::invalid_transport_security));
    }

    if (update && update->action == comic_chat::ircv3::StsPolicyAction::Upgrade) {
        if (connection.tls_verified || connection.transport.security != Security::plaintext ||
            update->port == 0 || update->duration != 0 || update->preload || !reconnect_secure) {
            return std::unexpected(Failure(StsSessionError::invalid_protocol_update));
        }
        // Do not touch this object after the callback: a native adapter may
        // synchronously close this session and start its secure replacement.
        bool reconnected{};
        try {
            reconnected = reconnect_secure(update->port);
        } catch (...) {
            return std::unexpected(Failure(StsSessionError::callback_failure));
        }
        if (!reconnected) {
            return std::unexpected(Failure(StsSessionError::callback_failure));
        }
        return StsProtocolDisposition::reconnected;
    }

    if (update) {
        if (!connection.tls_verified || connection.transport.security != Security::tls ||
            update->action == comic_chat::ircv3::StsPolicyAction::Upgrade) {
            return std::unexpected(Failure(StsSessionError::invalid_protocol_update));
        }
        auto applied = store_.apply_verified_update(
            connection.requested_hostname,
            connection.transport.endpoint.port,
            true,
            connection.generation,
            *update,
            now);
        if (!applied) return std::unexpected(StoreFailure(applied.error()));
        if (update->action == comic_chat::ircv3::StsPolicyAction::Persist) {
            if (!*applied) {
                healthy_ = false;
                return std::unexpected(Failure(StsSessionError::invalid_protocol_update));
            }
            connection.receipt = std::move(**applied);
        } else if (update->action == comic_chat::ircv3::StsPolicyAction::Remove) {
            connection.receipt.reset();
        } else {
            return std::unexpected(Failure(StsSessionError::invalid_protocol_update));
        }
    }

    if (!continue_output) {
        return std::unexpected(Failure(StsSessionError::callback_failure));
    }
    bool continued{};
    try {
        continued = continue_output();
    } catch (...) {
        return std::unexpected(Failure(StsSessionError::callback_failure));
    }
    if (!continued) return std::unexpected(Failure(StsSessionError::callback_failure));
    return StsProtocolDisposition::continued;
}

auto StsSessionPolicy::transport_disconnected(
    const GenerationId generation,
    const bool retain_for_retry,
    const StsTimePoint now) -> std::expected<void, StsSessionFailure> {
    const auto current = Current(generation);
    if (!current) return std::unexpected(current.error());
    auto& connection = **current;
    const auto requested_hostname = connection.requested_hostname;
    const bool tls_verified = connection.connected && connection.tls_verified;
    auto receipt = std::move(connection.receipt);

    connection.connected = false;
    connection.tls_verified = false;
    connection.receipt.reset();
    if (!retain_for_retry) active_.reset();

    if (!tls_verified) return {};
    const auto rescheduled = store_.reschedule_on_verified_disconnect(
        requested_hostname, true, generation, receipt, now);
    if (!rescheduled) return std::unexpected(StoreFailure(rescheduled.error()));
    return {};
}

auto StsSessionPolicy::active_generation() const noexcept -> GenerationId {
    return active_ ? active_->generation : 0;
}

auto StsSessionPolicy::active_options() const noexcept -> std::optional<StsTransportPlan> {
    return active_ ? std::optional<StsTransportPlan>{active_->transport} : std::nullopt;
}

auto StsSessionPolicy::has_persistence_receipt() const noexcept -> bool {
    return active_ && active_->receipt.has_value();
}

} // namespace comicchat::net
