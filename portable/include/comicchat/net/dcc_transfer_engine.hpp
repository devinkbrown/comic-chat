#pragma once

#include "comicchat/cpp26.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace comicchat::net {

using DccGenerationId = std::uint64_t;
using DccTransferId = std::uint64_t;
using DccPeerToken = std::uint64_t;

struct DccTransferHandle final {
    DccGenerationId generation{};
    DccTransferId transfer{};
    auto operator==(const DccTransferHandle&) const -> bool = default;
};

struct DccLimits final {
    std::uint64_t maximum_file_bytes{0xffff'ffffULL};
    std::size_t maximum_queued_send_bytes{256U * 1024U};
    std::size_t maximum_uncommitted_receive_bytes{256U * 1024U};
    std::size_t receive_chunk_bytes{16U * 1024U};
    std::size_t maximum_events{256};
    std::size_t maximum_commands{256};
};

struct DccDeadlines final {
    std::chrono::milliseconds connect{30'000};
    std::chrono::milliseconds accept{120'000};
    std::chrono::milliseconds idle{60'000};
};

// DCC SEND: listen for exactly one peer and stream bytes to it. A wildcard
// bind requires an explicit, validated IPv4 address to advertise over CTCP.
struct DccListenOptions final {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{};
    std::optional<std::string> advertise_address;
    std::optional<std::string> expected_peer_address;
    std::uint64_t file_size{};
    DccLimits limits;
    DccDeadlines deadlines;
};

// DCC RECEIVE: connect to the numeric address advertised by the sender. The
// adapter commits each received chunk only after its file write succeeds.
struct DccConnectOptions final {
    std::string peer_address;
    std::uint16_t port{};
    std::uint64_t file_size{};
    DccLimits limits;
    DccDeadlines deadlines;
};

struct DccQueueChunk final {
    DccTransferHandle handle;
    std::vector<std::byte> bytes;
    bool final{};
};
struct DccCommitReceived final {
    DccTransferHandle handle;
    std::uint64_t through_offset{};
};
struct DccAcceptPeer final {
    DccTransferHandle handle;
    DccPeerToken peer{};
};
struct DccRejectPeer final {
    DccTransferHandle handle;
    DccPeerToken peer{};
};
struct DccCancel final {
    DccTransferHandle handle;
    std::string reason;
};
using DccCommand = std::variant<DccQueueChunk, DccCommitReceived, DccAcceptPeer, DccRejectPeer, DccCancel>;

struct DccListening final {
    std::string bound_address;
    std::string advertise_address;
    std::uint16_t port{};
    std::uint32_t legacy_ipv4_decimal{};
};
// An inbound socket is inert until the adapter explicitly accepts this token.
// Rejecting it leaves the one-shot listener available for the intended peer.
struct DccPeerOffered final { DccPeerToken peer{}; std::string peer_address; };
struct DccPeerConnected final { std::string peer_address; };
struct DccChunkReceived final {
    std::uint64_t offset{};
    std::shared_ptr<const std::vector<std::byte>> bytes;
};
struct DccProgress final {
    std::uint64_t transferred{};
    std::uint64_t peer_committed{};
    std::uint64_t total{};
};
struct DccWritableCredit final { std::size_t bytes{}; };
struct DccCompleted final { std::uint64_t bytes{}; };
struct DccClosed final { std::string reason; };
struct DccDiagnostic final { std::string code; std::string message; };
using DccEventBody = std::variant<DccListening, DccPeerOffered, DccPeerConnected, DccChunkReceived, DccProgress,
                                  DccWritableCredit, DccCompleted, DccClosed, DccDiagnostic>;
struct DccEvent final {
    DccTransferHandle handle;
    DccEventBody body;
};

enum class DccError {
    already_running,
    not_running,
    stale_transfer,
    queue_full,
    invalid_address,
    invalid_options,
    protocol_error,
};

[[nodiscard]] auto dcc_legacy_ipv4_decimal(std::string_view address)
    -> std::expected<std::uint32_t, DccError>;

class DccTransferEngine final {
public:
    DccTransferEngine();
    ~DccTransferEngine();
    DccTransferEngine(const DccTransferEngine&) = delete;
    auto operator=(const DccTransferEngine&) -> DccTransferEngine& = delete;
    DccTransferEngine(DccTransferEngine&&) noexcept;
    auto operator=(DccTransferEngine&&) noexcept -> DccTransferEngine&;

    [[nodiscard]] auto start_listen(DccListenOptions options)
        -> std::expected<DccTransferHandle, DccError>;
    [[nodiscard]] auto start_connect(DccConnectOptions options)
        -> std::expected<DccTransferHandle, DccError>;
    [[nodiscard]] auto post(DccCommand command) -> std::expected<void, DccError>;
    [[nodiscard]] auto poll_events(std::size_t maximum = 128) -> std::vector<DccEvent>;
    void set_wakeup(std::function<void()> wakeup);
    void stop() noexcept;
    [[nodiscard]] auto handle() const noexcept -> DccTransferHandle;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace comicchat::net
