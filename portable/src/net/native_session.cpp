#include "comicchat/net/native_session.hpp"

#include "comicchat/memory.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

#include <mbedtls/platform_util.h>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace comicchat::net {
namespace {

constexpr std::size_t maximum_transport_events_per_poll = 128;
constexpr std::size_t maximum_protocol_items_per_poll = 1024;
constexpr std::size_t maximum_lines_per_receive_event = 512;
constexpr std::size_t maximum_lines_per_poll = 512;
constexpr std::size_t maximum_protocol_wire_bytes = 8191U + 512U;

void secure_clear(std::string& value) noexcept {
    if (!value.empty()) mbedtls_platform_zeroize(value.data(), value.size());
    value.clear();
}

struct ConditionalStringWipe final {
    std::string* value{};
    bool enabled{};
    ~ConditionalStringWipe() {
        if (enabled && value != nullptr) secure_clear(*value);
    }
};

struct OptionalStringWipe final {
    std::optional<std::string>* value{};
    ~OptionalStringWipe() {
        if (value != nullptr && *value) secure_clear(**value);
    }
};

struct SessionOptionsWipe final {
    NativeSessionOptions* options{};
    ~SessionOptionsWipe() {
        if (options == nullptr) return;
        secure_clear(options->sasl.password);
        if (options->connection.proxy.password) secure_clear(*options->connection.proxy.password);
    }
};

auto now_seconds() -> StsTimePoint {
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

auto safe_atom(const std::string_view value, const std::size_t maximum) -> bool {
    return !value.empty() && value.size() <= maximum &&
           std::ranges::none_of(value, [](const unsigned char byte) { return byte <= 0x20U || byte == 0x7fU; });
}

auto registration_line(std::string command, std::vector<std::string> params) -> std::optional<std::string> {
    comic_chat::ircv3::Message message;
    message.command = std::move(command);
    message.params = std::move(params);
    auto serialized = message.SerializeChecked(false);
    if (!serialized || serialized->size() > maximum_protocol_wire_bytes) return std::nullopt;
    return std::move(*serialized);
}

auto priority_for(const std::string_view wire) noexcept -> Priority {
    if (wire.starts_with("PONG ") || wire == "PONG\r\n") return Priority::pong;
    if (wire.starts_with("AUTHENTICATE ")) return Priority::authentication;
    if (wire.starts_with("PRIVMSG ") || wire.starts_with("NOTICE ")) return Priority::chat;
    return Priority::control;
}

auto sensitive_line(const std::string_view wire) noexcept -> bool {
    return wire.starts_with("AUTHENTICATE ");
}

class ConnectionSessionTransport final : public SessionTransport {
  public:
    auto start(ConnectionOptions options) -> std::expected<GenerationId, EngineError> override {
        const OptionalStringWipe wipe{&options.proxy.password};
        return engine_.start(options);
    }
    auto post(Command command) -> std::expected<void, EngineError> override { return engine_.post(std::move(command)); }
    auto poll_events(const std::size_t maximum) -> std::vector<Event> override { return engine_.poll_events(maximum); }
    void set_wakeup(std::function<void()> wakeup) override { engine_.set_wakeup(std::move(wakeup)); }
    void stop() noexcept override { engine_.stop(); }

  private:
    ConnectionEngine engine_;
};

#if !defined(_WIN32)

// prepare_native_user_paths (the #else arm below) is the only consumer; on
// Windows that function short-circuits to home_unavailable without touching
// the filesystem, so these would be unused-and-Werror-fatal at file scope.
constexpr std::string_view application_directory = "comic-chat-reinked";
constexpr std::string_view sts_policy_filename = "sts-policies-v1";

class UniqueFd final {
  public:
    explicit UniqueFd(const int value = -1) noexcept : value_{value} {}
    ~UniqueFd() {
        if (value_ >= 0) (void)::close(value_);
    }
    UniqueFd(const UniqueFd&) = delete;
    auto operator=(const UniqueFd&) -> UniqueFd& = delete;
    UniqueFd(UniqueFd&& other) noexcept : value_{std::exchange(other.value_, -1)} {}
    auto operator=(UniqueFd&& other) noexcept -> UniqueFd& {
        if (this == &other) return *this;
        if (value_ >= 0) (void)::close(value_);
        value_ = std::exchange(other.value_, -1);
        return *this;
    }
    [[nodiscard]] auto get() const noexcept -> int { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ >= 0; }

  private:
    int value_;
};

auto component_error(const int error) noexcept -> NativePathError {
    if (error == ELOOP || error == ENOTDIR) return NativePathError::unsafe_component;
    if (error == EACCES || error == EPERM) return NativePathError::wrong_owner;
    return NativePathError::io_error;
}

auto open_or_create_directory(const std::filesystem::path& requested, const bool private_final)
    -> std::expected<UniqueFd, NativePathError> {
    if (!requested.is_absolute() || requested.empty()) return std::unexpected{NativePathError::invalid_base};
    const auto path = requested.lexically_normal();
    if (path == path.root_path()) return std::unexpected{NativePathError::invalid_base};

    UniqueFd parent{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (!parent) return std::unexpected{NativePathError::io_error};

    std::vector<std::string> components;
    for (const auto& component : path.relative_path()) {
        const auto name = component.string();
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos)
            return std::unexpected{NativePathError::unsafe_component};
        components.push_back(name);
    }
    if (components.empty()) return std::unexpected{NativePathError::invalid_base};

    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto& component = components[index];
        int descriptor = ::openat(parent.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor < 0 && errno == ENOENT) {
            if (::mkdirat(parent.get(), component.c_str(), 0700) != 0 && errno != EEXIST)
                return std::unexpected{component_error(errno)};
            descriptor = ::openat(parent.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
        if (descriptor < 0) return std::unexpected{component_error(errno)};
        UniqueFd child{descriptor};
        struct stat status{};
        if (::fstat(child.get(), &status) != 0) return std::unexpected{NativePathError::io_error};
        if (!S_ISDIR(status.st_mode)) return std::unexpected{NativePathError::unsafe_component};
        if (private_final && index + 1 == components.size()) {
            if (status.st_uid != ::geteuid()) return std::unexpected{NativePathError::wrong_owner};
            if (::fchmod(child.get(), 0700) != 0) return std::unexpected{NativePathError::io_error};
            if (::fstat(child.get(), &status) != 0 || (status.st_mode & 0777) != 0700)
                return std::unexpected{NativePathError::io_error};
        }
        parent = std::move(child);
    }
    return parent;
}

auto account_home() -> std::expected<std::filesystem::path, NativePathError> {
    long suggested = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (suggested < 1024) suggested = 16U * 1024U;
    constexpr long maximum = 1024L * 1024L;
    suggested = std::min(suggested, maximum);
    std::vector<char> buffer(static_cast<std::size_t>(suggested));
    struct passwd account{};
    struct passwd* result{};
    const int status = ::getpwuid_r(::geteuid(), &account, buffer.data(), buffer.size(), &result);
    if (status != 0 || result == nullptr || account.pw_dir == nullptr || account.pw_dir[0] == '\0')
        return std::unexpected{NativePathError::home_unavailable};
    std::filesystem::path home{account.pw_dir};
    if (!home.is_absolute()) return std::unexpected{NativePathError::home_unavailable};
    return home;
}

auto environment_base(const char* name, const std::filesystem::path& fallback)
    -> std::expected<std::filesystem::path, NativePathError> {
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') {
        std::filesystem::path path{value};
        if (!path.is_absolute()) return std::unexpected{NativePathError::invalid_base};
        return path;
    }
    return fallback;
}

#endif

} // namespace

auto prepare_native_user_paths(std::optional<std::filesystem::path> config_base,
                               std::optional<std::filesystem::path> cache_base)
    -> std::expected<NativeUserPaths, NativePathError> {
#if defined(_WIN32)
    (void)config_base;
    (void)cache_base;
    return std::unexpected{NativePathError::home_unavailable};
#else
    if (::getuid() != ::geteuid() || ::getgid() != ::getegid())
        return std::unexpected{NativePathError::privileged_process};

    if (!config_base || !cache_base) {
        const auto home = account_home();
        if (!home) return std::unexpected{home.error()};
        if (!config_base) {
            auto discovered = environment_base("XDG_CONFIG_HOME", *home / ".config");
            if (!discovered) return std::unexpected{discovered.error()};
            config_base = std::move(*discovered);
        }
        if (!cache_base) {
            auto discovered = environment_base("XDG_CACHE_HOME", *home / ".cache");
            if (!discovered) return std::unexpected{discovered.error()};
            cache_base = std::move(*discovered);
        }
    }

    const auto config_root = open_or_create_directory(*config_base, false);
    if (!config_root) return std::unexpected{config_root.error()};
    const auto cache_root = open_or_create_directory(*cache_base, false);
    if (!cache_root) return std::unexpected{cache_root.error()};

    NativeUserPaths paths;
    paths.config_directory = *config_base / application_directory;
    paths.cache_directory = *cache_base / application_directory;
    const auto private_config = open_or_create_directory(paths.config_directory, true);
    if (!private_config) return std::unexpected{private_config.error()};
    const auto private_cache = open_or_create_directory(paths.cache_directory, true);
    if (!private_cache) return std::unexpected{private_cache.error()};
    paths.sts_policy_file = paths.config_directory / sts_policy_filename;
    return paths;
#endif
}

auto make_connection_session_transport() -> std::unique_ptr<SessionTransport> {
    return std::make_unique<ConnectionSessionTransport>();
}

class NativeSession::Impl final {
  public:
    Impl(std::filesystem::path sts_policy_file, std::unique_ptr<SessionTransport> transport, Now now)
        : store_{std::move(sts_policy_file)}, transport_{std::move(transport)},
          now_{now ? std::move(now) : Now{now_seconds}}, protocol_{std::make_unique<comic_chat::ircv3::Engine>()} {}

    ~Impl() {
        if (transport_) {
            try {
                transport_->set_wakeup({});
            } catch (...) {
            }
        }
        stop();
    }

    auto start(NativeSessionOptions options) -> std::expected<GenerationId, NativeSessionError> {
        const SessionOptionsWipe wipe{&options};
        if (running_) return std::unexpected{NativeSessionError::already_running};
        if (!healthy_) return std::unexpected{NativeSessionError::sts_store};
        if (!transport_ || !safe_atom(options.connection.endpoint.host, 253) || options.connection.endpoint.port == 0 ||
            !safe_atom(options.nickname, 64) || (!options.channel.empty() && !safe_atom(options.channel, 512))) {
            secure_clear(options.sasl.password);
            return std::unexpected{NativeSessionError::invalid_options};
        }

        std::optional<SharedLockedSecret> password;
        if (!options.sasl.password.empty()) {
            auto locked = LockedSecret::copy(options.sasl.password);
            secure_clear(options.sasl.password);
            if (!locked) {
                return std::unexpected{locked.error() == SecretError::lock_failed
                                           ? NativeSessionError::credential_lock_failed
                                           : NativeSessionError::invalid_options};
            }
            auto shared = std::move(*locked).share();
            if (!shared) return std::unexpected{NativeSessionError::invalid_options};
            password.emplace(std::move(*shared));
        }

        std::optional<LockedSecret> proxy_password;
        if (options.connection.proxy.password) {
            if (options.connection.proxy.password->empty()) {
                options.connection.proxy.password.reset();
            } else {
                auto locked = LockedSecret::copy(*options.connection.proxy.password);
                secure_clear(*options.connection.proxy.password);
                options.connection.proxy.password.reset();
                if (!locked) {
                    return std::unexpected{locked.error() == SecretError::lock_failed
                                               ? NativeSessionError::credential_lock_failed
                                               : NativeSessionError::invalid_options};
                }
                proxy_password.emplace(std::move(*locked));
            }
        }

        if (!store_.load(now_())) return std::unexpected{NativeSessionError::sts_store};
        auto planned = store_.plan(options.connection, now_());
        if (!planned) return std::unexpected{NativeSessionError::sts_store};

        requested_connection_ = options.connection;
        requested_hostname_ = options.connection.server_name.empty()
                                  ? options.connection.endpoint.host
                                  : options.connection.server_name;
        current_connection_ = planned->options;
        sasl_template_ = std::move(options.sasl);
        secure_clear(sasl_template_.password);
        password_ = std::move(password);
        proxy_password_ = std::move(proxy_password);
        nickname_ = std::move(options.nickname);
        channel_ = std::move(options.channel);
        protocol_ = std::make_unique<comic_chat::ircv3::Engine>();
        framer_.Reset();
        receipt_.reset();
        connected_ = false;
        tls_verified_ = false;
        joined_ = false;
        next_send_id_ = 1;

        auto started = start_transport(current_connection_);
        if (!started) {
            password_.reset();
            proxy_password_.reset();
            return std::unexpected{started.error() == EngineError::credential_lock_failed
                                       ? NativeSessionError::credential_lock_failed
                                       : NativeSessionError::transport};
        }
        generation_ = *started;
        running_ = true;
        return generation_;
    }

    auto poll(const std::size_t maximum_transport_events, const std::size_t maximum_protocol_items)
        -> NativeSessionPoll {
        NativeSessionPoll output;
        std::size_t remaining = std::min(maximum_protocol_items, maximum_protocol_items_per_poll);
        std::size_t remaining_lines = maximum_lines_per_poll;
        const auto maximum = std::min(maximum_transport_events, maximum_transport_events_per_poll);
        if (running_ && maximum != 0) {
            auto events = transport_->poll_events(maximum);
            for (auto& event : events) {
                if (event.generation != generation_) continue;
                std::visit([&](auto&& body) {
                    handle_body(std::forward<decltype(body)>(body), output, remaining, remaining_lines);
                },
                           std::move(event.body));
            }
        }
        output.connected = connected_;
        output.tls_verified = tls_verified_;
        return output;
    }

    void set_wakeup(std::function<void()> wakeup) {
        if (transport_) transport_->set_wakeup(std::move(wakeup));
    }

    auto send_privmsg(std::string target, std::string text) -> std::expected<void, NativeSessionError> {
        if (!running_ || !transport_) return std::unexpected{NativeSessionError::not_running};
        if (!safe_atom(target, 512)) return std::unexpected{NativeSessionError::invalid_message};
        auto wire = registration_line("PRIVMSG", {target, std::move(text)});
        if (!wire) return std::unexpected{NativeSessionError::invalid_message};
        if (!queue_protocol_line(std::move(*wire), std::move(target)))
            return std::unexpected{NativeSessionError::transport};
        return {};
    }

    void stop() noexcept {
        if (!transport_) return;
        if (running_ && connected_ && tls_verified_) {
            try {
                const auto rescheduled = store_.reschedule_on_verified_disconnect(
                    requested_hostname_, true, generation_, receipt_, now_());
                if (!rescheduled) healthy_ = false;
            } catch (...) {
                healthy_ = false;
            }
        }
        transport_->stop();
        running_ = false;
        connected_ = false;
        tls_verified_ = false;
        receipt_.reset();
        protocol_.reset();
        password_.reset();
        proxy_password_.reset();
        generation_ = 0;
    }

    [[nodiscard]] auto generation() const noexcept -> GenerationId { return generation_; }
    [[nodiscard]] auto running() const noexcept -> bool { return running_; }

  private:
    enum class LineResult { complete, restarted, failed };

    static void append_diagnostic(NativeSessionPoll& output, std::size_t& remaining, std::string code,
                                  std::string message) {
        if (remaining == 0) {
            ++output.dropped_protocol_items;
            return;
        }
        --remaining;
        output.diagnostics.push_back({std::move(code), std::move(message)});
    }

    static void append_state(NativeSessionPoll& output, std::size_t& remaining, const State state) {
        if (remaining == 0) {
            ++output.dropped_protocol_items;
            return;
        }
        --remaining;
        output.states.push_back(state);
    }

    // `target` is only ever the bounded per-target fairness hint carried on
    // the Send command (see connection_engine.hpp); it is never interpreted
    // as a network address. Non-chat protocol lines (registration, JOIN,
    // PING/PONG replies, ...) leave it empty, matching prior behavior.
    auto queue_protocol_line(std::string wire, std::string target = {}) -> bool {
        const bool sensitive = sensitive_line(wire);
        const ConditionalStringWipe wire_wipe{&wire, sensitive};
        const auto priority = priority_for(wire);
        auto prepared = protocol_->PrepareOutgoingChecked(wire);
        const ConditionalStringWipe prepared_wipe{prepared ? &*prepared : nullptr, sensitive};
        if (!prepared || prepared->empty() || prepared->size() > maximum_protocol_wire_bytes) {
            return false;
        }

        std::vector<std::byte> bytes;
        bytes.reserve(prepared->size());
        for (const unsigned char byte : *prepared)
            bytes.push_back(static_cast<std::byte>(byte));
        Send command{generation_, next_send_id_++, priority, std::move(bytes), sensitive, std::move(target)};
        return transport_->post(std::move(command)).has_value();
    }

    auto start_transport(ConnectionOptions options) -> std::expected<GenerationId, EngineError> {
        if (proxy_password_) {
            const auto view = proxy_password_->view();
            options.proxy.password.emplace(reinterpret_cast<const char*>(view.data()), view.size());
        }
        const OptionalStringWipe wipe{&options.proxy.password};
        return transport_->start(options);
    }

    auto queue_registration() -> bool {
        auto sasl = sasl_template_;
        auto commands = protocol_->BeginRegistration(
            std::move(sasl), password_ ? *password_ : SharedLockedSecret{}, nickname_, tls_verified_);
        for (auto& command : commands) {
            if (!queue_protocol_line(std::move(command))) return false;
        }
        auto nick = registration_line("NICK", {nickname_});
        auto user = registration_line("USER", {nickname_, "0", "*", nickname_});
        return nick && user && queue_protocol_line(std::move(*nick)) && queue_protocol_line(std::move(*user));
    }

    void disconnect_protocol_failure() {
        if (!running_ || generation_ == 0) return;
        (void)transport_->post(Disconnect{generation_, "protocol policy failure"});
    }

    void fail_closed_sts() noexcept {
        healthy_ = false;
        if (transport_) transport_->stop();
        running_ = false;
        connected_ = false;
        tls_verified_ = false;
        receipt_.reset();
        joined_ = false;
        framer_.Reset();
        protocol_.reset();
        password_.reset();
        proxy_password_.reset();
        generation_ = 0;
    }

    auto restart_secure(const std::uint16_t port) -> bool {
        ConnectionOptions secure = requested_connection_;
        secure.security = Security::tls;
        secure.endpoint.port = port;
        if (secure.server_name.empty()) secure.server_name = secure.endpoint.host;

        transport_->stop();
        running_ = false;
        connected_ = false;
        tls_verified_ = false;
        receipt_.reset();
        joined_ = false;
        framer_.Reset();
        protocol_ = std::make_unique<comic_chat::ircv3::Engine>();
        current_connection_ = secure;
        auto started = start_transport(std::move(secure));
        if (!started) {
            generation_ = 0;
            return false;
        }
        generation_ = *started;
        running_ = true;
        return true;
    }

    template <typename Value>
    static void append_protocol_item(std::vector<Value>& destination, Value value, NativeSessionPoll& output,
                                     std::size_t& remaining) {
        if (remaining == 0) {
            ++output.dropped_protocol_items;
            return;
        }
        --remaining;
        destination.push_back(std::move(value));
    }

    auto process_line(std::string_view line, NativeSessionPoll& output, std::size_t& remaining) -> LineResult {
        auto result = protocol_->Process(line);
        if (result.sts_update) {
            const auto& update = *result.sts_update;
            if (update.action == comic_chat::ircv3::StsPolicyAction::Upgrade) {
                for (auto& event : result.events)
                    append_protocol_item(output.protocol_events, std::move(event), output, remaining);
                if (tls_verified_ || update.port == 0 || !restart_secure(update.port)) {
                    append_diagnostic(output, remaining, "sts-upgrade-failed",
                                      "STS secure restart could not be established");
                    return LineResult::failed;
                }
                return LineResult::restarted;
            }
            if (!tls_verified_) {
                append_diagnostic(output, remaining, "sts-unverified-update",
                                  "unverified STS persistence update was ignored");
            } else {
                auto applied =
                    store_.apply_verified_update(requested_hostname_, current_connection_.endpoint.port,
                                                 true, generation_, update, now_());
                if (!applied) {
                    append_diagnostic(output, remaining, "sts-store-failed",
                                      "verified STS policy could not be committed");
                    fail_closed_sts();
                    return LineResult::failed;
                } else {
                    receipt_ = std::move(*applied);
                }
            }
        }

        for (auto& event : result.events)
            append_protocol_item(output.protocol_events, std::move(event), output, remaining);
        for (auto& message : result.messages) {
            if (!joined_ && message.command == "001" && !channel_.empty()) {
                auto join = registration_line("JOIN", {channel_});
                if (!join || !queue_protocol_line(std::move(*join))) {
                    append_diagnostic(output, remaining, "join-queue-failed",
                                      "initial channel join could not be queued");
                    disconnect_protocol_failure();
                    return LineResult::failed;
                }
                joined_ = true;
            }
            append_protocol_item(output.messages, std::move(message), output, remaining);
        }
        for (auto& outbound : result.outbound) {
            const ConditionalStringWipe outbound_wipe{&outbound, sensitive_line(outbound)};
            if (!queue_protocol_line(outbound)) {
                append_diagnostic(output, remaining, "protocol-queue-failed",
                                  "IRC protocol response could not be queued");
                disconnect_protocol_failure();
                return LineResult::failed;
            }
        }
        return LineResult::complete;
    }

    void handle_body(StateChanged body, NativeSessionPoll& output, std::size_t& remaining, std::size_t&) {
        append_state(output, remaining, body.state);
    }

    void handle_body(const Connected& body, NativeSessionPoll& output, std::size_t& remaining, std::size_t&) {
        if (connected_) {
            append_diagnostic(output, remaining, "transport-phase-violation",
                              "transport reported a duplicate connected event");
            disconnect_protocol_failure();
            return;
        }
        if (current_connection_.security == Security::tls && !body.tls) {
            append_diagnostic(output, remaining, "tls-downgrade-blocked",
                              "transport reported plaintext for a TLS-only generation");
            disconnect_protocol_failure();
            return;
        }
        connected_ = true;
        tls_verified_ = body.tls;
        receipt_.reset();
        joined_ = false;
        framer_.Reset();
        protocol_ = std::make_unique<comic_chat::ircv3::Engine>();
        if (!queue_registration()) {
            append_diagnostic(output, remaining, "registration-queue-failed", "IRC registration could not be queued");
            disconnect_protocol_failure();
        }
    }

    void handle_body(const BytesReceived& body, NativeSessionPoll& output, std::size_t& remaining,
                     std::size_t& remaining_lines) {
        if (!body.bytes) return;
        if (!connected_) {
            append_diagnostic(output, remaining, "transport-phase-violation",
                              "transport delivered IRC bytes outside a connected phase");
            disconnect_protocol_failure();
            return;
        }
        auto lines = framer_.Push(std::span<const std::byte>{*body.bytes});
        if (!lines || lines->size() > maximum_lines_per_receive_event) {
            append_diagnostic(output, remaining, "invalid-frame", "IRC framing limit was exceeded");
            disconnect_protocol_failure();
            return;
        }
        if (lines->size() > remaining_lines) {
            append_diagnostic(output, remaining, "protocol-work-limit",
                              "IRC line work exceeded the bounded poll budget");
            disconnect_protocol_failure();
            return;
        }
        remaining_lines -= lines->size();
        for (const auto& line : *lines) {
            const auto result = process_line(line, output, remaining);
            if (result != LineResult::complete) return;
        }
    }

    void handle_body(SendComplete, NativeSessionPoll&, std::size_t&, std::size_t&) {}

    void handle_body(PingDue, NativeSessionPoll& output, std::size_t& remaining, std::size_t&) {
        if (!connected_) return;
        auto ping = protocol_->PrepareKeepalivePing();
        if (!ping || !queue_protocol_line(std::move(*ping))) {
            append_diagnostic(output, remaining, "keepalive-failed", "IRC keepalive could not be queued");
            disconnect_protocol_failure();
        }
    }

    void handle_body(const Closed& body, NativeSessionPoll& output, std::size_t& remaining, std::size_t&) {
        bool reschedule_failed{};
        if (connected_ && tls_verified_) {
            const auto rescheduled = store_.reschedule_on_verified_disconnect(
                requested_hostname_, true, generation_, receipt_, now_());
            if (!rescheduled) {
                append_diagnostic(output, remaining, "sts-reschedule-failed",
                                  "verified STS duration could not be rescheduled");
                reschedule_failed = true;
            }
        }
        if (reschedule_failed) {
            fail_closed_sts();
            return;
        }
        connected_ = false;
        tls_verified_ = false;
        receipt_.reset();
        joined_ = false;
        framer_.Reset();
        if (body.retry_after <= std::chrono::milliseconds::zero()) running_ = false;
    }

    void handle_body(Diagnostic body, NativeSessionPoll& output, std::size_t& remaining, std::size_t&) {
        append_diagnostic(output, remaining, std::move(body.code), std::move(body.message));
    }

    StsPolicyStore store_;
    std::unique_ptr<SessionTransport> transport_;
    Now now_;
    std::unique_ptr<comic_chat::ircv3::Engine> protocol_;
    comic_chat::ircv3::LineFramer framer_;
    ConnectionOptions requested_connection_;
    ConnectionOptions current_connection_;
    std::string requested_hostname_;
    comic_chat::ircv3::SaslConfig sasl_template_;
    std::optional<SharedLockedSecret> password_;
    std::optional<LockedSecret> proxy_password_;
    std::optional<StsPolicyReceipt> receipt_;
    std::string nickname_;
    std::string channel_;
    GenerationId generation_{};
    SendId next_send_id_{1};
    bool running_{};
    bool connected_{};
    bool tls_verified_{};
    bool joined_{};
    bool healthy_{true};
};

NativeSession::NativeSession(std::filesystem::path sts_policy_file, std::unique_ptr<SessionTransport> transport,
                             Now now)
    : impl_{std::make_unique<Impl>(std::move(sts_policy_file), std::move(transport), std::move(now))} {}

NativeSession::~NativeSession() = default;
NativeSession::NativeSession(NativeSession&&) noexcept = default;
auto NativeSession::operator=(NativeSession&&) noexcept -> NativeSession& = default;

auto NativeSession::start(NativeSessionOptions options) -> std::expected<GenerationId, NativeSessionError> {
    if (!impl_) return std::unexpected{NativeSessionError::transport};
    return impl_->start(std::move(options));
}

auto NativeSession::poll(const std::size_t maximum_transport_events, const std::size_t maximum_protocol_items)
    -> NativeSessionPoll {
    return impl_ ? impl_->poll(maximum_transport_events, maximum_protocol_items) : NativeSessionPoll{};
}

void NativeSession::set_wakeup(std::function<void()> wakeup) {
    if (impl_) impl_->set_wakeup(std::move(wakeup));
}

auto NativeSession::send_privmsg(std::string target, std::string text) -> std::expected<void, NativeSessionError> {
    if (!impl_) return std::unexpected{NativeSessionError::not_running};
    return impl_->send_privmsg(std::move(target), std::move(text));
}

void NativeSession::stop() noexcept {
    if (impl_) impl_->stop();
}
auto NativeSession::generation() const noexcept -> GenerationId {
    return impl_ ? impl_->generation() : 0;
}
auto NativeSession::running() const noexcept -> bool {
    return impl_ && impl_->running();
}

} // namespace comicchat::net
