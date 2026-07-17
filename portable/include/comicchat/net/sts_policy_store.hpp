#pragma once

#include "comicchat/cpp26.hpp"
#include "comicchat/net/connection_engine.hpp"
#include "comicchat/net/ircv3.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace comicchat::net {

using StsTimePoint = std::chrono::sys_seconds;

enum class StsStoreError : std::uint8_t {
    not_loaded,
    invalid_hostname,
    invalid_port,
    invalid_update,
    clock_overflow,
    file_too_large,
    too_many_policies,
    malformed_file,
    unsafe_file,
    io_error,
};

struct StsPolicyRecord final {
    std::string hostname;
    std::uint16_t secure_port{};
    std::uint64_t duration_seconds{};
    StsTimePoint expires_at{};
    bool preload{};
};

struct StsConnectionPlan final {
    ConnectionOptions options;
    bool enforced{};
};

class StsPolicyReceipt final {
public:
    [[nodiscard]] auto hostname() const noexcept -> std::string_view { return hostname_; }
    [[nodiscard]] auto duration_seconds() const noexcept -> std::uint64_t { return duration_seconds_; }
    [[nodiscard]] auto generation() const noexcept -> GenerationId { return generation_; }

private:
    friend class StsPolicyStore;
    StsPolicyReceipt(const StsPolicyRecord& policy, const GenerationId generation)
        : hostname_(policy.hostname), duration_seconds_(policy.duration_seconds), generation_(generation) {}

    std::string hostname_;
    std::uint64_t duration_seconds_{};
    GenerationId generation_{};
};

// Each object still has one external owner (normally the UI/session thread),
// while durable mutations are coordinated with other processes using the same
// store. The caller supplies a file inside an already-created, per-user private
// configuration directory. This class deliberately does not guess HOME,
// LOCALAPPDATA, XDG paths, create parent directories, or retain secrets.
class StsPolicyStore final {
public:
    static constexpr std::size_t maximum_file_bytes = std::size_t{256} * 1024U;
    static constexpr std::size_t maximum_policies = 1024;
    static constexpr std::size_t maximum_hostname_bytes = 255;

    explicit StsPolicyStore(std::filesystem::path file);

    // A missing file is a valid empty store. Any other read or parse error is
    // fail-closed and leaves the previous in-memory snapshot unchanged.
    [[nodiscard]] auto load(StsTimePoint now) -> std::expected<void, StsStoreError>;

    [[nodiscard]] auto find(std::string_view requested_hostname, StsTimePoint now) const
        -> std::expected<std::optional<StsPolicyRecord>, StsStoreError>;

    // Must run before ConnectionEngine::start. An active policy replaces a
    // caller's plaintext request with TLS on the last verified secure port.
    [[nodiscard]] auto plan(ConnectionOptions requested, StsTimePoint now) const
        -> std::expected<StsConnectionPlan, StsStoreError>;

    // Applies only typed Persist/Remove updates received on a verified TLS
    // connection. The durable file and in-memory snapshot change together.
    [[nodiscard]] auto apply_verified_update(
        std::string_view requested_hostname,
        std::uint16_t connected_secure_port,
        bool tls_verified,
        GenerationId connection_generation,
        const comic_chat::ircv3::StsPolicyUpdate& update,
        StsTimePoint now) -> std::expected<std::optional<StsPolicyReceipt>, StsStoreError>;

    // IRCv3 STS requires rebasing the last advertised duration when the secure
    // connection closes, even when the original expiry passed while connected.
    // The receipt must be the last Persist receipt retained by that same live
    // connection. No receipt means that connection advertised no persistence
    // policy and therefore must not extend an older cached policy.
    [[nodiscard]] auto reschedule_on_verified_disconnect(
        std::string_view requested_hostname,
        bool tls_verified,
        GenerationId closing_generation,
        const std::optional<StsPolicyReceipt>& last_persistence_from_this_connection,
        StsTimePoint now) -> std::expected<void, StsStoreError>;

    [[nodiscard]] auto loaded() const noexcept -> bool { return loaded_; }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return policies_.size(); }
    [[nodiscard]] auto file() const noexcept -> const std::filesystem::path& { return file_; }

private:
    using Policies = std::map<std::string, StsPolicyRecord, std::less<>>;

    struct PersistResult final {
        std::optional<StsStoreError> error;
        bool replaced{};
    };

    [[nodiscard]] auto persist(const Policies& policies) -> PersistResult;

    std::filesystem::path file_;
    Policies policies_;
    bool loaded_{};
};

} // namespace comicchat::net
