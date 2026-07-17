#include "comicchat/net/connection_engine.hpp"
#include "comicchat/net/dcc_transfer_engine.hpp"
#include "comicchat/net/private_config.hpp"
#include "comicchat/net/sts_policy_store.hpp"
#include "comicchat/net/sts_session.hpp"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <utility>
#include <vector>

// This translation unit is deliberately compiled by both the portable Clang
// gate and the Windows nmake build. It catches namespace/API drift in the MFC
// adapters without requiring MFC headers in the portable build.
namespace {

using comicchat::net::ConnectionEngine;
using comicchat::net::DccTransferEngine;
using comicchat::net::StsPolicyStore;
using comicchat::net::StsSessionPolicy;

static_assert(std::same_as<
    decltype(std::declval<ConnectionEngine&>().start(
        std::declval<comicchat::net::ConnectionOptions>())),
    std::expected<comicchat::net::GenerationId, comicchat::net::EngineError>>);
static_assert(std::same_as<
    decltype(std::declval<ConnectionEngine&>().post(
        std::declval<comicchat::net::Command>())),
    std::expected<void, comicchat::net::EngineError>>);
static_assert(std::same_as<
    decltype(std::declval<ConnectionEngine&>().poll_events(std::size_t{128})),
    std::vector<comicchat::net::Event>>);
static_assert(std::same_as<
    decltype(std::declval<ConnectionEngine&>().set_wakeup(
        std::declval<std::function<void()>>())),
    void>);
static_assert(noexcept(std::declval<ConnectionEngine&>().stop()));

static_assert(std::same_as<
    decltype(std::declval<DccTransferEngine&>().start_listen(
        std::declval<comicchat::net::DccListenOptions>())),
    std::expected<comicchat::net::DccTransferHandle, comicchat::net::DccError>>);
static_assert(std::same_as<
    decltype(std::declval<DccTransferEngine&>().start_connect(
        std::declval<comicchat::net::DccConnectOptions>())),
    std::expected<comicchat::net::DccTransferHandle, comicchat::net::DccError>>);
static_assert(std::same_as<
    decltype(std::declval<DccTransferEngine&>().post(
        std::declval<comicchat::net::DccCommand>())),
    std::expected<void, comicchat::net::DccError>>);
static_assert(std::same_as<
    decltype(std::declval<DccTransferEngine&>().poll_events(std::size_t{128})),
    std::vector<comicchat::net::DccEvent>>);
static_assert(std::same_as<
    decltype(comicchat::net::dcc_ipv4_scope(std::declval<std::string_view>())),
    std::expected<comicchat::net::DccAddressScope, comicchat::net::DccError>>);
static_assert(noexcept(std::declval<DccTransferEngine&>().stop()));

static_assert(std::same_as<
    decltype(std::declval<const StsPolicyStore&>().plan(
        std::declval<comicchat::net::ConnectionOptions>(),
        std::declval<comicchat::net::StsTimePoint>())),
    std::expected<comicchat::net::StsConnectionPlan, comicchat::net::StsStoreError>>);
static_assert(std::same_as<
    decltype(std::declval<StsSessionPolicy&>().start(
        std::declval<comicchat::net::ConnectionOptions>(),
        std::declval<comicchat::net::StsTimePoint>(),
        std::declval<const StsSessionPolicy::StartTransport&>())),
    std::expected<comicchat::net::StsSessionStart, comicchat::net::StsSessionFailure>>);
static_assert(std::same_as<
    decltype(std::declval<StsSessionPolicy&>().connected(
        std::declval<comicchat::net::GenerationId>(), true)),
    std::expected<void, comicchat::net::StsSessionFailure>>);
static_assert(std::same_as<
    decltype(std::declval<StsSessionPolicy&>().transport_disconnected(
        std::declval<comicchat::net::GenerationId>(), true,
        std::declval<comicchat::net::StsTimePoint>())),
    std::expected<void, comicchat::net::StsSessionFailure>>);
static_assert(std::same_as<
    decltype(comicchat::net::native_private_config_file(std::declval<std::string_view>())),
    std::expected<std::filesystem::path, comicchat::net::PrivateConfigError>>);

} // namespace
