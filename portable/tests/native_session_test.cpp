#include "comicchat/memory.hpp"
#include "comicchat/net/native_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {

using namespace std::chrono_literals;
using comicchat::net::BytesReceived;
using comicchat::net::Closed;
using comicchat::net::Command;
using comicchat::net::Connected;
using comicchat::net::ConnectionOptions;
using comicchat::net::Diagnostic;
using comicchat::net::EngineError;
using comicchat::net::Event;
using comicchat::net::GenerationId;
using comicchat::net::NativeSession;
using comicchat::net::NativeSessionOptions;
using comicchat::net::Security;
using comicchat::net::SessionTransport;
using comicchat::net::StsPolicyStore;
using comicchat::net::StsTimePoint;

auto at(const std::int64_t seconds) -> StsTimePoint {
    return StsTimePoint{std::chrono::seconds{seconds}};
}

struct TemporaryDirectory final {
    TemporaryDirectory() {
        static std::uint64_t sequence{};
        const auto root = std::filesystem::temp_directory_path();
        for (unsigned int attempt = 0; attempt < 100; ++attempt) {
            path = root / ("comicchat-native-session-" + std::to_string(++sequence) + "-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) return;
        }
        path.clear();
    }
    ~TemporaryDirectory() {
        std::error_code error;
        if (!path.empty()) std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

class FakeTransport final : public SessionTransport {
  public:
    auto start(ConnectionOptions options) -> std::expected<GenerationId, EngineError> override {
        starts.push_back(std::move(options));
        running = true;
        return ++next_generation;
    }
    auto post(Command command) -> std::expected<void, EngineError> override {
        if (!running) return std::unexpected{EngineError::not_running};
        posts.push_back(std::move(command));
        return {};
    }
    auto poll_events(const std::size_t maximum) -> std::vector<Event> override {
        std::vector<Event> result;
        while (!events.empty() && result.size() < maximum) {
            result.push_back(std::move(events.front()));
            events.pop_front();
        }
        return result;
    }
    void set_wakeup(std::function<void()> callback) override { wakeup = std::move(callback); }
    void stop() noexcept override { running = false; }

    void push(Event event) {
        events.push_back(std::move(event));
        if (wakeup) wakeup();
    }

    std::uint64_t next_generation{};
    bool running{};
    std::vector<ConnectionOptions> starts;
    std::vector<Command> posts;
    std::deque<Event> events;
    std::function<void()> wakeup;
};

auto options(const Security security = Security::tls) -> NativeSessionOptions {
    NativeSessionOptions result;
    result.connection.endpoint = {"irc.example", security == Security::tls ? std::uint16_t{6697} : std::uint16_t{6667}};
    result.connection.server_name = "irc.example";
    result.connection.security = security;
    result.nickname = "Alice";
    result.channel = "#comic-chat";
    return result;
}

auto bytes(const std::string& value) -> std::shared_ptr<const std::vector<std::byte>> {
    auto result = std::make_shared<std::vector<std::byte>>();
    result->reserve(value.size());
    for (const unsigned char byte : value)
        result->push_back(static_cast<std::byte>(byte));
    return result;
}

TEST_CASE("native user paths create private config and cache directories "
          "without symlink traversal") {
#if defined(_WIN32)
    SUCCEED("POSIX state directory coverage runs on Linux and BSD");
#else
    TemporaryDirectory temporary;
    REQUIRE_FALSE(temporary.path.empty());
    const auto config_base = temporary.path / "config";
    const auto cache_base = temporary.path / "cache";
    const auto paths = comicchat::net::prepare_native_user_paths(config_base, cache_base);
    REQUIRE(paths);
    CHECK(paths->sts_policy_file == paths->config_directory / "sts-policies-v1");
    struct stat config_status{};
    struct stat cache_status{};
    REQUIRE(::stat(paths->config_directory.c_str(), &config_status) == 0);
    REQUIRE(::stat(paths->cache_directory.c_str(), &cache_status) == 0);
    CHECK((config_status.st_mode & 0777) == 0700);
    CHECK((cache_status.st_mode & 0777) == 0700);

    const auto real_base = temporary.path / "real";
    REQUIRE(std::filesystem::create_directory(real_base));
    const auto linked_base = temporary.path / "linked";
    std::error_code symlink_error;
    std::filesystem::create_directory_symlink(real_base, linked_base, symlink_error);
    REQUIRE_FALSE(symlink_error);
    CHECK_FALSE(comicchat::net::prepare_native_user_paths(linked_base, cache_base));
#endif
}

TEST_CASE("durable STS is planned before the transport generation starts") {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    StsPolicyStore writer{file};
    REQUIRE(writer.load(at(100)));
    const comic_chat::ircv3::StsPolicyUpdate persist{comic_chat::ircv3::StsPolicyAction::Persist, 0, 3600, false};
    REQUIRE(writer.apply_verified_update("irc.example", 7443, true, 1, persist, at(100)));

    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{file, std::move(fake), [] { return at(200); }};
    const auto started = session.start(options(Security::plaintext));
    REQUIRE(started);
    REQUIRE(observed->starts.size() == 1);
    CHECK(observed->starts[0].security == Security::tls);
    CHECK(observed->starts[0].endpoint.port == 7443);
}

TEST_CASE("plaintext STS upgrade restarts securely before any response from "
          "that line") {
    TemporaryDirectory temporary;
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{temporary.path / "sts-policies", std::move(fake), [] { return at(100); }};
    REQUIRE(session.start(options(Security::plaintext)));
    observed->push(Event{1, Connected{"irc.example", "127.0.0.1", false, false}});
    (void)session.poll();
    observed->posts.clear();

    observed->push(Event{1, BytesReceived{bytes(":server CAP Alice LS :sts=port=7443 sasl\r\n")}});
    (void)session.poll();

    REQUIRE(observed->starts.size() == 2);
    CHECK(observed->starts.back().security == Security::tls);
    CHECK(observed->starts.back().endpoint.port == 7443);
    CHECK(observed->posts.empty());
    CHECK(session.generation() == 2);
}

TEST_CASE("only verified TLS persistence receives a disconnect reschedule receipt") {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    auto now = at(100);
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{file, std::move(fake), [&] { return now; }};
    REQUIRE(session.start(options()));
    observed->push(Event{1, Connected{"irc.example", "127.0.0.1", true, false}});
    (void)session.poll();
    observed->push(Event{1, BytesReceived{bytes(":server CAP Alice LS :sts=duration=10\r\n")}});
    (void)session.poll();

    now = at(200);
    observed->push(Event{1, Closed{"transport failure", 500ms}});
    (void)session.poll();

    StsPolicyStore restarted{file};
    REQUIRE(restarted.load(at(205)));
    const auto policy = restarted.find("irc.example", at(205));
    REQUIRE(policy);
    REQUIRE(policy->has_value());
    CHECK((*policy)->expires_at == at(210));
}

TEST_CASE("native STS persistence is scoped to the requested TLS hostname") {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{file, std::move(fake), [] { return at(100); }};
    auto request = options();
    request.connection.endpoint.host = "203.0.113.10";
    request.connection.server_name = "irc.example";
    REQUIRE(session.start(std::move(request)));
    observed->push(Event{1, Connected{"203.0.113.10", "127.0.0.1", true, false}});
    (void)session.poll();
    observed->push(Event{1, BytesReceived{bytes(":server CAP Alice LS :sts=duration=3600\r\n")}});
    (void)session.poll();

    StsPolicyStore restarted{file};
    REQUIRE(restarted.load(at(101)));
    const auto requested_host = restarted.find("irc.example", at(101));
    const auto endpoint_address = restarted.find("203.0.113.10", at(101));
    REQUIRE(requested_host);
    REQUIRE(requested_host->has_value());
    REQUIRE(endpoint_address);
    CHECK_FALSE(endpoint_address->has_value());
}

TEST_CASE("native STS write failure blocks same-line output and latches fail closed") {
    TemporaryDirectory temporary;
    const auto state_directory = temporary.path / "state";
    REQUIRE(std::filesystem::create_directory(state_directory));
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{state_directory / "sts-policies", std::move(fake), [] { return at(100); }};
    REQUIRE(session.start(options()));
    observed->push(Event{1, Connected{"irc.example", "127.0.0.1", true, false}});
    (void)session.poll();
    observed->posts.clear();
    std::error_code remove_error;
    REQUIRE(std::filesystem::remove_all(state_directory, remove_error) > 0);
    REQUIRE_FALSE(remove_error);

    observed->push(Event{1, BytesReceived{bytes(":server CAP Alice LS :sts=duration=3600 standard-replies\r\n")}});
    const auto failed = session.poll();

    REQUIRE(failed.diagnostics.size() == 1);
    CHECK(failed.diagnostics.front().code == "sts-store-failed");
    CHECK(failed.protocol_events.empty());
    CHECK(failed.messages.empty());
    CHECK(observed->posts.empty());
    CHECK_FALSE(observed->running);
    CHECK_FALSE(session.running());
    CHECK(session.generation() == 0);

    REQUIRE(std::filesystem::create_directory(state_directory));
    const auto retry = session.start(options(Security::plaintext));
    REQUIRE_FALSE(retry);
    CHECK(retry.error() == comicchat::net::NativeSessionError::sts_store);
    CHECK(observed->starts.size() == 1);
}

TEST_CASE("native STS rebase failure cancels automatic retry and latches fail closed") {
    TemporaryDirectory temporary;
    const auto state_directory = temporary.path / "state";
    REQUIRE(std::filesystem::create_directory(state_directory));
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{state_directory / "sts-policies", std::move(fake), [] { return at(100); }};
    REQUIRE(session.start(options()));
    observed->push(Event{1, Connected{"irc.example", "127.0.0.1", true, false}});
    (void)session.poll();
    observed->push(Event{1, BytesReceived{bytes(":server CAP Alice LS :sts=duration=10\r\n")}});
    (void)session.poll();
    std::error_code remove_error;
    REQUIRE(std::filesystem::remove_all(state_directory, remove_error) > 0);
    REQUIRE_FALSE(remove_error);

    observed->push(Event{1, Closed{"transport failure", 500ms}});
    const auto failed = session.poll();

    REQUIRE(failed.diagnostics.size() == 1);
    CHECK(failed.diagnostics.front().code == "sts-reschedule-failed");
    CHECK_FALSE(observed->running);
    CHECK_FALSE(session.running());
    CHECK(session.generation() == 0);

    REQUIRE(std::filesystem::create_directory(state_directory));
    const auto retry = session.start(options(Security::plaintext));
    REQUIRE_FALSE(retry);
    CHECK(retry.error() == comicchat::net::NativeSessionError::sts_store);
    CHECK(observed->starts.size() == 1);
}

TEST_CASE("plaintext duration advertisements cannot create durable policy") {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{file, std::move(fake), [] { return at(100); }};
    REQUIRE(session.start(options(Security::plaintext)));
    observed->push(Event{1, Connected{"irc.example", "127.0.0.1", false, false}});
    observed->push(Event{1, BytesReceived{bytes(":server CAP Alice LS :sts=duration=3600\r\n")}});
    (void)session.poll();

    StsPolicyStore restarted{file};
    REQUIRE(restarted.load(at(101)));
    CHECK(restarted.size() == 0);
}

TEST_CASE("session polling is bounded and discards stale generations") {
    TemporaryDirectory temporary;
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{temporary.path / "sts-policies", std::move(fake), [] { return at(100); }};
    REQUIRE(session.start(options()));
    observed->push(Event{0, Diagnostic{"stale", "must not escape"}});
    for (int index = 0; index < 5; ++index) {
        observed->push(Event{1, Diagnostic{"bounded", "fixed diagnostic"}});
    }
    const auto first = session.poll(2);
    CHECK(first.diagnostics.size() == 1);
    const auto second = session.poll(2);
    CHECK(second.diagnostics.size() == 2);
    CHECK(observed->events.size() == 2);
}

TEST_CASE("a TLS-only generation rejects a plaintext connected event") {
    TemporaryDirectory temporary;
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{temporary.path / "sts-policies", std::move(fake), [] { return at(100); }};
    REQUIRE(session.start(options(Security::tls)));
    observed->push(Event{1, Connected{"irc.example", "127.0.0.1", false, false}});
    const auto result = session.poll();

    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "tls-downgrade-blocked");
    REQUIRE(observed->posts.size() == 1);
    CHECK(std::holds_alternative<comicchat::net::Disconnect>(observed->posts.front()));
}

TEST_CASE("session credentials fail closed when native page locking is unavailable") {
    TemporaryDirectory temporary;
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{temporary.path / "sts-policies", std::move(fake), [] { return at(100); }};
    auto request = options(Security::tls);
    request.sasl.authentication_id = "Alice";
    request.sasl.password = "secret";
    comicchat::testing::fail_next_secret_lock();
    const auto started = session.start(std::move(request));

    REQUIRE_FALSE(started);
    CHECK(started.error() == comicchat::net::NativeSessionError::credential_lock_failed);
    CHECK(observed->starts.empty());
}

TEST_CASE("an excessive IRC line burst closes before parsing any line") {
    TemporaryDirectory temporary;
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{temporary.path / "sts-policies", std::move(fake), [] { return at(100); }};
    REQUIRE(session.start(options(Security::plaintext)));
    observed->push(Event{1, Connected{"irc.example", "127.0.0.1", false, false}});
    (void)session.poll();
    observed->posts.clear();

    std::string burst;
    for (std::size_t index = 0; index < 513; ++index)
        burst += "\r\n";
    observed->push(Event{1, BytesReceived{bytes(burst)}});
    const auto result = session.poll();

    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "invalid-frame");
    REQUIRE(observed->posts.size() == 1);
    CHECK(std::holds_alternative<comicchat::net::Disconnect>(observed->posts.front()));
}

TEST_CASE("IRC bytes are rejected before connected and after closed transport phases") {
    TemporaryDirectory temporary;
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{temporary.path / "sts-policies", std::move(fake), [] { return at(100); }};
    REQUIRE(session.start(options(Security::plaintext)));

    observed->push(Event{1, BytesReceived{bytes("PING :premature\r\n")}});
    const auto premature = session.poll();
    REQUIRE(premature.diagnostics.size() == 1);
    CHECK(premature.diagnostics.front().code == "transport-phase-violation");
    CHECK(premature.messages.empty());
    REQUIRE(observed->posts.size() == 1);
    CHECK(std::holds_alternative<comicchat::net::Disconnect>(observed->posts.back()));

    observed->posts.clear();
    observed->push(Event{1, Connected{"irc.example", "127.0.0.1", false, false}});
    (void)session.poll();
    observed->posts.clear();
    observed->push(Event{1, Closed{"transport failure", 500ms}});
    observed->push(Event{1, BytesReceived{bytes("PING :late\r\n")}});
    const auto late = session.poll();
    REQUIRE(late.diagnostics.size() == 1);
    CHECK(late.diagnostics.front().code == "transport-phase-violation");
    CHECK(late.messages.empty());
    REQUIRE(observed->posts.size() == 1);
    CHECK(std::holds_alternative<comicchat::net::Disconnect>(observed->posts.back()));
}

TEST_CASE("native poll line-work budget spans all receive events") {
    TemporaryDirectory temporary;
    auto fake = std::make_unique<FakeTransport>();
    auto* observed = fake.get();
    NativeSession session{temporary.path / "sts-policies", std::move(fake), [] { return at(100); }};
    REQUIRE(session.start(options(Security::plaintext)));
    observed->push(Event{1, Connected{"irc.example", "127.0.0.1", false, false}});
    (void)session.poll();
    observed->posts.clear();

    std::string first_burst;
    std::string second_burst;
    for (std::size_t index = 0; index < 256; ++index) {
        first_burst += ":server NOTICE Alice :first\r\n";
        second_burst += ":server NOTICE Alice :second\r\n";
    }
    second_burst += ":server NOTICE Alice :over-budget\r\n";
    observed->push(Event{1, BytesReceived{bytes(first_burst)}});
    observed->push(Event{1, BytesReceived{bytes(second_burst)}});
    const auto result = session.poll();

    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics.front().code == "protocol-work-limit");
    CHECK_FALSE(result.messages.empty());
    CHECK(result.messages.size() <= 256);
    for (const auto& message : result.messages) {
        REQUIRE_FALSE(message.params.empty());
        CHECK(message.params.back() == "first");
    }
    REQUIRE(observed->posts.size() == 1);
    CHECK(std::holds_alternative<comicchat::net::Disconnect>(observed->posts.back()));
}

} // namespace
