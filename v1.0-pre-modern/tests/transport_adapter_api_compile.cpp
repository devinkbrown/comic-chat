#include "comicchat/crypto_runtime.hpp"
#include "comicchat/net/connection_engine.hpp"
#include "comicchat/net/dcc_transfer_engine.hpp"
#include "comicchat/net/ircv3.hpp"

#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// This client-local translation unit deliberately contains no runtime adapter
// and no MFC dependency.  Both MSVC's v1 product link and the portable Clang
// gate compile it, so the exact shared APIs that the upcoming v1 adapter will
// consume cannot drift unnoticed while the legacy socket remains active.
namespace {

using comicchat::net::ConnectionEngine;
using comicchat::net::DccTransferEngine;
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

static_assert(
    std::same_as<decltype(comicchat::crypto::initialize_runtime()), bool>);
static_assert(noexcept(comicchat::crypto::initialize_runtime()));
static_assert(std::same_as<decltype(comicchat::crypto::random_bytes(
                               std::declval<std::span<std::byte>>())),
                           bool>);
static_assert(noexcept(
    comicchat::crypto::random_bytes(std::declval<std::span<std::byte>>())));

} // namespace
