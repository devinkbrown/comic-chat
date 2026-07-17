#include "comicchat/crypto_runtime.hpp"
#include "comicchat/net/connection_engine.hpp"
#include "comicchat/net/dcc_transfer_engine.hpp"
#include "comicchat/net/ircv3.hpp"
#include "comicchat/net/private_config.hpp"
#include "comicchat/net/sts_policy_store.hpp"
#include "comicchat/net/sts_session.hpp"
#include "../transportadapter.h"

#include <concepts>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// This client-local translation unit deliberately contains no MFC dependency.
// Both MSVC's v1 product link and the portable Clang gate compile it, so the
// exact APIs consumed by the live plain-class v1 adapter cannot drift.
namespace {

using comicchat::net::ConnectionEngine;
using comicchat::net::DccTransferEngine;
using comicchat::net::StsPolicyStore;
using comicchat::net::StsSessionPolicy;
using IrcEngine = comic_chat::ircv3::Engine;

static_assert(
    std::same_as<decltype(std::declval<ConnectionEngine &>().start(
                     std::declval<comicchat::net::ConnectionOptions>())),
                 std::expected<comicchat::net::GenerationId,
                               comicchat::net::EngineError>>);
static_assert(std::same_as<decltype(std::declval<ConnectionEngine &>().post(
                               std::declval<comicchat::net::Command>())),
                           std::expected<void, comicchat::net::EngineError>>);
static_assert(std::same_as<decltype(std::declval<ConnectionEngine &>()
                                        .poll_events(std::size_t{128})),
                           std::vector<comicchat::net::Event>>);
static_assert(
    std::same_as<decltype(std::declval<ConnectionEngine &>().set_wakeup(
                     std::declval<std::function<void()>>())),
                 void>);
static_assert(noexcept(std::declval<ConnectionEngine &>().stop()));

static_assert(
    std::same_as<decltype(std::declval<DccTransferEngine &>().start_listen(
                     std::declval<comicchat::net::DccListenOptions>())),
                 std::expected<comicchat::net::DccTransferHandle,
                               comicchat::net::DccError>>);
static_assert(
    std::same_as<decltype(std::declval<DccTransferEngine &>().start_connect(
                     std::declval<comicchat::net::DccConnectOptions>())),
                 std::expected<comicchat::net::DccTransferHandle,
                               comicchat::net::DccError>>);
static_assert(std::same_as<decltype(std::declval<DccTransferEngine &>().post(
                               std::declval<comicchat::net::DccCommand>())),
                           std::expected<void, comicchat::net::DccError>>);
static_assert(std::same_as<decltype(std::declval<DccTransferEngine &>()
                                        .poll_events(std::size_t{128})),
                           std::vector<comicchat::net::DccEvent>>);
static_assert(noexcept(std::declval<DccTransferEngine &>().stop()));

static_assert(std::same_as<decltype(comic_chat::ircv3::Message::Parse(
                               std::declval<std::string_view>())),
                           std::expected<comic_chat::ircv3::Message,
                                         comic_chat::ircv3::ParseFailure>>);
static_assert(std::same_as<decltype(std::declval<IrcEngine &>().Process(
                               std::declval<std::string_view>())),
                           comic_chat::ircv3::ProcessResult>);
static_assert(
    std::same_as<decltype(std::declval<IrcEngine &>().PrepareOutgoingChecked(
                     std::declval<std::string_view>())),
                 std::expected<std::string, comic_chat::ircv3::ParseFailure>>);
static_assert(std::same_as<decltype(std::declval<IrcEngine &>()
                                        .FinishRegistrationAfterTimeout()),
                           std::vector<std::string>>);

static_assert(std::same_as<decltype(std::declval<StsPolicyStore &>().load(
								   std::declval<comicchat::net::StsTimePoint>())),
				   std::expected<void, comicchat::net::StsStoreError>>);
static_assert(std::same_as<decltype(std::declval<const StsPolicyStore &>().plan(
								   std::declval<comicchat::net::ConnectionOptions>(),
								   std::declval<comicchat::net::StsTimePoint>())),
				   std::expected<comicchat::net::StsConnectionPlan,
							 comicchat::net::StsStoreError>>);
static_assert(std::same_as<decltype(std::declval<StsSessionPolicy &>().start(
								   std::declval<comicchat::net::ConnectionOptions>(),
								   std::declval<comicchat::net::StsTimePoint>(),
								   std::declval<const StsSessionPolicy::StartTransport &>())),
				   std::expected<comicchat::net::StsSessionStart,
							 comicchat::net::StsSessionFailure>>);
static_assert(std::same_as<decltype(std::declval<StsSessionPolicy &>().connected(
								   comicchat::net::GenerationId{1}, true)),
				   std::expected<void, comicchat::net::StsSessionFailure>>);
static_assert(std::same_as<decltype(std::declval<StsSessionPolicy &>()
								   .route_protocol_update(
									   std::declval<const std::optional<
										   comic_chat::ircv3::StsPolicyUpdate> &>(),
									   comicchat::net::GenerationId{1},
									   std::declval<comicchat::net::StsTimePoint>(),
									   std::declval<const StsSessionPolicy::ReconnectSecure &>(),
									   std::declval<const StsSessionPolicy::ContinueProtocolOutput &>())),
				   std::expected<comicchat::net::StsProtocolDisposition,
							 comicchat::net::StsSessionFailure>>);
static_assert(std::same_as<decltype(std::declval<StsSessionPolicy &>()
								   .transport_disconnected(
									   comicchat::net::GenerationId{1}, true,
									   std::declval<comicchat::net::StsTimePoint>())),
				   std::expected<void, comicchat::net::StsSessionFailure>>);
static_assert(std::same_as<decltype(comicchat::net::native_private_config_file(
								   std::declval<std::string_view>())),
				   std::expected<std::filesystem::path,
							 comicchat::net::PrivateConfigError>>);

static_assert(std::same_as<decltype(comic_chat::v1::transport::PrepareOutbound(
								   std::declval<IrcEngine &>(),
								   std::declval<std::string_view>(),
								   comicchat::net::GenerationId{1},
								   comicchat::net::SendId{1})),
				   std::expected<comicchat::net::Send,
				 comic_chat::v1::transport::AdapterError>>);
static_assert(std::same_as<decltype(comic_chat::v1::transport::PrepareLegacyInbound(
								   std::declval<const comic_chat::ircv3::Message &>())),
				   std::expected<std::string,
								 comic_chat::v1::transport::AdapterError>>);
static_assert(std::same_as<decltype(comic_chat::v1::transport::PrepareLegacyInbound(
								   std::declval<const comic_chat::ircv3::Message &>(),
								   std::declval<std::string_view>())),
				   std::expected<std::string,
								 comic_chat::v1::transport::AdapterError>>);
static_assert(std::same_as<decltype(
	comic_chat::v1::transport::PrepareExplicitNamesRequest(
		true, std::declval<std::string_view>())), std::optional<std::string>>);
static_assert(std::same_as<decltype(comic_chat::v1::transport::BuildLegacyOutbound(
								   std::declval<std::string_view>(),
								   std::declval<std::span<const std::string_view>>(),
								   std::declval<std::optional<std::string_view>>())),
				   std::expected<std::string,
							 comic_chat::v1::transport::AdapterError>>);
static_assert(std::same_as<decltype(comic_chat::v1::transport::BuildLegacyOutbound(
								   std::declval<std::string_view>(),
								   std::declval<std::span<const std::string_view>>(),
								   std::declval<std::span<const std::string_view>>())),
				   std::expected<std::string,
							 comic_chat::v1::transport::AdapterError>>);
static_assert(noexcept(std::declval<comic_chat::v1::transport::SessionGate &>().Stop()));
static_assert(noexcept(std::declval<comic_chat::v1::transport::WakeupGate &>().Disable()));
static_assert(std::same_as<decltype(std::declval<
	comic_chat::v1::transport::ProtocolLineBudget &>().Consume(std::size_t{1})), bool>);
static_assert(noexcept(std::declval<
	comic_chat::v1::transport::ProtocolLineBudget &>().Consume(std::size_t{1})));

static_assert(
    std::same_as<decltype(comicchat::crypto::initialize_runtime()), bool>);
static_assert(noexcept(comicchat::crypto::initialize_runtime()));
static_assert(std::same_as<decltype(comicchat::crypto::random_bytes(
                               std::declval<std::span<std::byte>>())),
                           bool>);
static_assert(noexcept(
    comicchat::crypto::random_bytes(std::declval<std::span<std::byte>>())));

} // namespace
