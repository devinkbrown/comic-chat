#pragma once

#include "comicchat/cpp26.hpp"
#include "comicchat/net/connection_engine.hpp"
#include "comicchat/net/ircv3.hpp"
#include "comicchat/net/sts_policy_store.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace comicchat::net {

enum class NativePathError {
    privileged_process,
    home_unavailable,
    invalid_base,
    unsafe_component,
    wrong_owner,
    io_error,
};

struct NativeUserPaths final {
    std::filesystem::path config_directory;
    std::filesystem::path cache_directory;
    std::filesystem::path sts_policy_file;
};

// Creates private, process-native per-user directories without following
// symlinks. Empty overrides select XDG_CONFIG_HOME/XDG_CACHE_HOME and then the
// account database's home directory; tests may supply absolute roots.
[[nodiscard]] auto prepare_native_user_paths(std::optional<std::filesystem::path> config_base = std::nullopt,
                                             std::optional<std::filesystem::path> cache_base = std::nullopt)
    -> std::expected<NativeUserPaths, NativePathError>;

class SessionTransport {
  public:
    virtual ~SessionTransport() = default;
    [[nodiscard]] virtual auto start(ConnectionOptions options) -> std::expected<GenerationId, EngineError> = 0;
    [[nodiscard]] virtual auto post(Command command) -> std::expected<void, EngineError> = 0;
    [[nodiscard]] virtual auto poll_events(std::size_t maximum) -> std::vector<Event> = 0;
    virtual void set_wakeup(std::function<void()> wakeup) = 0;
    virtual void stop() noexcept = 0;
};

[[nodiscard]] auto make_connection_session_transport() -> std::unique_ptr<SessionTransport>;

struct NativeSessionOptions final {
    ConnectionOptions connection;
    comic_chat::ircv3::SaslConfig sasl;
    std::string nickname;
    std::string channel;
};

enum class NativeSessionError {
    already_running,
    invalid_options,
    credential_lock_failed,
    sts_store,
    transport,
    // The session is not currently started (post()/send_privmsg() called
    // before start() or after stop()/a fail-closed shutdown).
    not_running,
    // The target or text could not be framed as a valid IRC line (embedded
    // control bytes, an empty/oversized target, or a line that would exceed
    // the negotiated wire-frame budget).
    invalid_message,
};

struct NativeSessionDiagnostic final {
    std::string code;
    std::string message;
};

struct NativeSessionPoll final {
    std::vector<State> states;
    std::vector<comic_chat::ircv3::Message> messages;
    std::vector<comic_chat::ircv3::Event> protocol_events;
    std::vector<NativeSessionDiagnostic> diagnostics;
    std::size_t dropped_protocol_items{};
    bool connected{};
    bool tls_verified{};
};

// NativeSession is not internally synchronized: start()/poll()/set_wakeup()/
// stop()/send_privmsg() must all be called from the single external thread
// that owns the session (the same thread already required to call poll()).
// That external thread only ever hands commands to the transport/engine,
// which does its own locking against its private network thread, so this
// restriction costs nothing in the app's single-threaded event-loop use.
class NativeSession final {
  public:
    using Now = std::function<StsTimePoint()>;

    explicit NativeSession(std::filesystem::path sts_policy_file,
                           std::unique_ptr<SessionTransport> transport = make_connection_session_transport(),
                           Now now = {});
    ~NativeSession();
    NativeSession(const NativeSession&) = delete;
    auto operator=(const NativeSession&) -> NativeSession& = delete;
    NativeSession(NativeSession&&) noexcept;
    auto operator=(NativeSession&&) noexcept -> NativeSession&;

    [[nodiscard]] auto start(NativeSessionOptions options) -> std::expected<GenerationId, NativeSessionError>;
    [[nodiscard]] auto poll(std::size_t maximum_transport_events = 64, std::size_t maximum_protocol_items = 512)
        -> NativeSessionPoll;
    void set_wakeup(std::function<void()> wakeup);
    void stop() noexcept;

    // Queues an outbound PRIVMSG to `target` (a channel or nick) over the
    // live wire, going through the same IRCv3 outgoing pipeline
    // (PrepareOutgoingChecked) that registration and protocol-response lines
    // use, so labeled-response bookkeeping and echo-message dedup see this
    // line exactly like any other client-initiated message. Returns
    // not_running if the session is not currently started; the caller is
    // expected to keep its own local echo (this call never blocks on the
    // network and never retries).
    [[nodiscard]] auto send_privmsg(std::string target, std::string text)
        -> std::expected<void, NativeSessionError>;

    [[nodiscard]] auto generation() const noexcept -> GenerationId;
    [[nodiscard]] auto running() const noexcept -> bool;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace comicchat::net
