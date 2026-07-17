#include "comicchat/net/private_config.hpp"
#include "comicchat/net/sts_session.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using comic_chat::ircv3::StsPolicyAction;
using comic_chat::ircv3::StsPolicyUpdate;
using comicchat::net::ConnectionOptions;
using comicchat::net::GenerationId;
using comicchat::net::Security;
using comicchat::net::StsProtocolDisposition;
using comicchat::net::StsSessionError;
using comicchat::net::StsSessionPolicy;
using comicchat::net::StsTimePoint;

int failures{};

void Check(const bool condition, const std::string_view description) {
    if (condition) return;
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
}

auto At(const std::int64_t seconds) -> StsTimePoint {
    return StsTimePoint{std::chrono::seconds{seconds}};
}

struct TemporaryDirectory final {
    TemporaryDirectory() {
        static std::uint64_t sequence{};
        std::error_code error;
        const auto root = std::filesystem::temp_directory_path(error);
        if (error) return;
        for (unsigned int attempt = 0; attempt < 100; ++attempt) {
            path = root / ("comicchat-sts-session-test-" + std::to_string(++sequence) + "-" +
                           std::to_string(attempt));
            if (std::filesystem::create_directory(path, error)) return;
            error.clear();
        }
        path.clear();
    }
    ~TemporaryDirectory() {
        std::error_code error;
        if (!path.empty()) std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

auto PolicyFile(const TemporaryDirectory& temporary) -> std::filesystem::path {
    const auto path = comicchat::net::private_config_file_from_root(
        temporary.path, "sts-policies-v1");
    Check(path.has_value(), "trusted per-user root creates a private application directory");
    return path ? *path : std::filesystem::path{};
}

auto Request(std::string host, const std::uint16_t port, const Security security)
    -> ConnectionOptions {
    ConnectionOptions options;
    options.endpoint.host = host;
    options.endpoint.port = port;
    options.server_name = std::move(host);
    options.security = security;
    return options;
}

auto Persist(const std::uint64_t duration) -> StsPolicyUpdate {
    return {StsPolicyAction::Persist, 0, duration, false};
}

auto Remove() -> StsPolicyUpdate {
    return {StsPolicyAction::Remove, 0, 0, false};
}

void TestRestartAndRetriesNeverDowngrade() {
    TemporaryDirectory temporary;
    const auto file = PolicyFile(temporary);

    StsSessionPolicy first{file};
    Check(first.load(At(10)).has_value(), "first session loads empty durable state");
    bool first_start_called{};
    const auto first_started = first.start(
        Request("IRC.Example.", 7443, Security::tls), At(10),
        [&](ConnectionOptions planned) {
            first_start_called = true;
            Check(planned.security == Security::tls && planned.endpoint.port == 7443,
                  "explicit secure start reaches transport unchanged");
            return std::expected<GenerationId, comicchat::net::EngineError>{GenerationId{11}};
        });
    Check(first_start_called && first_started && first_started->generation == 11,
          "policy coordinator invokes the transport start");
    Check(first.connected(11, true).has_value(), "verified TLS marks the active transport");

    bool persistence_continued{};
    const auto persisted = first.route_protocol_update(
        Persist(3600), 11, At(12),
        [](std::uint16_t) { return false; },
        [&] {
            persistence_continued = true;
            Check(first.has_persistence_receipt(),
                  "secure persistence is committed before protocol output continues");
            return true;
        });
    Check(persisted && *persisted == StsProtocolDisposition::continued && persistence_continued,
          "verified persistence update continues the line after commit");
    Check(first.transport_disconnected(11, false, At(20)).has_value(),
          "verified disconnect reschedules and ends the first session");

    StsSessionPolicy restarted{file};
    Check(restarted.load(At(21)).has_value(), "new process reloads the durable policy");
    std::vector<ConnectionOptions> attempts;
    const auto enforced = restarted.start(
        Request("irc.example", 6667, Security::plaintext), At(21),
        [&](ConnectionOptions planned) {
            attempts.push_back(planned);
            return std::expected<GenerationId, comicchat::net::EngineError>{GenerationId{22}};
        });
    Check(enforced && enforced->enforced && attempts.size() == 1 &&
              attempts.front().security == Security::tls && attempts.front().endpoint.port == 7443,
          "restart plans TLS and the verified port before transport start");

    Check(restarted.transport_disconnected(22, true, At(22)).has_value(),
          "failed handshake retains the same planned retry session");
    Check(restarted.active_generation() == 22 &&
              restarted.active_options() && restarted.active_options()->security == Security::tls &&
              restarted.active_options()->endpoint.port == 7443,
          "internal retry retains enforced TLS options and cannot downgrade");
    Check(restarted.connected(22, true).has_value(),
          "same-generation verified retry can reconnect");
    Check(restarted.transport_disconnected(22, false, At(23)).has_value(),
          "external replacement ends the retry session");

    const auto replacement = restarted.start(
        Request("IRC.EXAMPLE", 6667, Security::plaintext), At(24),
        [&](ConnectionOptions planned) {
            attempts.push_back(planned);
            return std::expected<GenerationId, comicchat::net::EngineError>{GenerationId{23}};
        });
    Check(replacement && attempts.size() == 2 && attempts.back().security == Security::tls &&
              attempts.back().endpoint.port == 7443,
          "new external retry is replanned and still cannot downgrade");
}

void TestUpgradePreemptsOutputAndSecureRemovalClearsReceipt() {
    TemporaryDirectory temporary;
    const auto file = PolicyFile(temporary);
    StsSessionPolicy session{file};
    Check(session.load(At(100)).has_value(), "ordering session loads");
    const auto plaintext = session.start(
        Request("irc.example", 6667, Security::plaintext), At(100),
        [](ConnectionOptions) {
            return std::expected<GenerationId, comicchat::net::EngineError>{GenerationId{30}};
        });
    Check(plaintext.has_value() && session.connected(30, false).has_value(),
          "plaintext transport becomes active");

    std::vector<std::string> order;
    const StsPolicyUpdate upgrade{StsPolicyAction::Upgrade, 6697, 0, false};
    bool exception_output_called{};
    const auto callback_exception = session.route_protocol_update(
        upgrade, 30, At(101),
        [](std::uint16_t) -> bool { throw std::runtime_error{"test callback"}; },
        [&] { exception_output_called = true; return true; });
    Check(!callback_exception &&
              callback_exception.error().code == StsSessionError::callback_failure &&
              !exception_output_called && session.active_generation() == 30,
          "upgrade callback exception is contained without releasing output");
    const auto upgraded = session.route_protocol_update(
        upgrade, 30, At(101),
        [&](const std::uint16_t port) {
            order.emplace_back("reconnect:" + std::to_string(port));
            return true;
        },
        [&] {
            order.emplace_back("outbound");
            return true;
        });
    Check(upgraded && *upgraded == StsProtocolDisposition::reconnected &&
              order == std::vector<std::string>{"reconnect:6697"},
          "typed plaintext upgrade reconnects before and instead of same-line output");
    Check(session.transport_disconnected(30, false, At(101)).has_value(),
          "plaintext generation is cleared for secure replacement");

    const auto secure = session.start(
        Request("irc.example", 6697, Security::tls), At(102),
        [](ConnectionOptions) {
            return std::expected<GenerationId, comicchat::net::EngineError>{GenerationId{31}};
        });
    Check(secure.has_value() && session.connected(31, true).has_value(),
          "secure replacement becomes verified");
    const auto persist = session.route_protocol_update(
        Persist(600), 31, At(103), [](std::uint16_t) { return false; }, [&] {
            Check(session.has_persistence_receipt(), "persist receipt exists before output callback");
            return true;
        });
    Check(persist.has_value() && session.has_persistence_receipt(),
          "secure generation retains its own receipt");
    const auto output_exception = session.route_protocol_update(
        std::nullopt, 31, At(103), [](std::uint16_t) { return false; },
        []() -> bool { throw std::runtime_error{"test output"}; });
    Check(!output_exception &&
              output_exception.error().code == StsSessionError::callback_failure &&
              session.has_persistence_receipt(),
          "output callback exception is contained without losing the secure receipt");

    bool stale_reconnect_called{};
    bool stale_output_called{};
    const auto stale = session.route_protocol_update(
        Persist(900), 30, At(104),
        [&](std::uint16_t) { stale_reconnect_called = true; return true; },
        [&] { stale_output_called = true; return true; });
    Check(!stale && stale.error().code == StsSessionError::stale_generation &&
              !stale_reconnect_called && !stale_output_called && session.has_persistence_receipt(),
          "stale generation cannot mutate the current receipt or release output");

    const auto removed = session.route_protocol_update(
        Remove(), 31, At(105), [](std::uint16_t) { return false; }, [&] {
            Check(!session.has_persistence_receipt(),
                  "duration zero clears the current receipt before output continues");
            return true;
        });
    Check(removed && *removed == StsProtocolDisposition::continued &&
              !session.has_persistence_receipt(),
          "verified duration zero removes durable and connection-scoped policy");
    Check(session.transport_disconnected(31, false, At(106)).has_value(),
          "removed policy cannot be rescheduled at disconnect");

    StsSessionPolicy restarted{file};
    Check(restarted.load(At(107)).has_value(), "duration-zero store reloads");
    ConnectionOptions observed;
    const auto allowed = restarted.start(
        Request("irc.example", 6667, Security::plaintext), At(107),
        [&](ConnectionOptions planned) {
            observed = planned;
            return std::expected<GenerationId, comicchat::net::EngineError>{GenerationId{32}};
        });
    Check(allowed && !allowed->enforced && observed.security == Security::plaintext,
          "duration zero permits future plaintext after restart");
}

void TestUnreadableStateAndPersistenceFailureFailClosed() {
    TemporaryDirectory temporary;
    const auto file = PolicyFile(temporary);
    {
        std::ofstream malformed{file, std::ios::binary | std::ios::trunc};
        malformed << "not-an-sts-store\n";
    }
    StsSessionPolicy unreadable{file};
    const auto load = unreadable.load(At(200));
    Check(!load && load.error().code == StsSessionError::store_failure,
          "malformed durable state is rejected");
    bool started{};
    const auto blocked = unreadable.start(
        Request("irc.example", 6667, Security::plaintext), At(200),
        [&](ConnectionOptions) {
            started = true;
            return std::expected<GenerationId, comicchat::net::EngineError>{GenerationId{40}};
        });
    Check(!blocked && !started,
          "unreadable policy state blocks plaintext before transport start");

    std::error_code error;
    std::filesystem::remove(file, error);
    StsSessionPolicy failing{file};
    Check(failing.load(At(201)).has_value(), "fresh fail-closed store loads");
    const auto secure = failing.start(
        Request("irc.example", 6697, Security::tls), At(201),
        [](ConnectionOptions) {
            return std::expected<GenerationId, comicchat::net::EngineError>{GenerationId{41}};
        });
    Check(secure.has_value() && failing.connected(41, true).has_value(),
          "secure persistence failure fixture connects");
    std::filesystem::remove_all(file.parent_path(), error);
    bool output_released{};
    const auto rejected = failing.route_protocol_update(
        Persist(600), 41, At(202), [](std::uint16_t) { return false; },
        [&] { output_released = true; return true; });
    Check(!rejected && rejected.error().code == StsSessionError::store_failure &&
              !output_released && !failing.healthy(),
          "persistence failure blocks output and latches the session unhealthy");
}

} // namespace

int main() {
    TestRestartAndRetriesNeverDowngrade();
    TestUpgradePreemptsOutputAndSecureRemovalClearsReceipt();
    TestUnreadableStateAndPersistenceFailureFailClosed();
    if (failures != 0) {
        std::cerr << failures << " STS session test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "STS session tests passed\n";
    return EXIT_SUCCESS;
}
