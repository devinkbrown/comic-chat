#include "comicchat/net/sts_policy_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using comic_chat::ircv3::StsPolicyAction;
using comic_chat::ircv3::StsPolicyUpdate;
using comicchat::net::ConnectionOptions;
using comicchat::net::Security;
using comicchat::net::StsPolicyStore;
using comicchat::net::StsStoreError;
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
            path = root / ("comicchat-sts-test-" + std::to_string(++sequence) + "-" +
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

auto PlaintextRequest(std::string host = "irc.example", const std::uint16_t port = 6667)
    -> ConnectionOptions {
    ConnectionOptions options;
    options.endpoint.host = host;
    options.endpoint.port = port;
    options.server_name = std::move(host);
    options.security = Security::plaintext;
    return options;
}

auto Persist(const std::uint64_t duration, const bool preload = false) -> StsPolicyUpdate {
    return {StsPolicyAction::Persist, 0, duration, preload};
}

auto Remove() -> StsPolicyUpdate {
    return {StsPolicyAction::Remove, 0, 0, false};
}

auto Hex(const std::string_view input) -> std::string {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    for (const char raw : input) {
        const auto byte = static_cast<unsigned char>(raw);
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

void TestDurableHostnamePolicyAndNoDowngrade() {
    TemporaryDirectory temporary;
    Check(!temporary.path.empty(), "temporary directory created");
    const auto file = temporary.path / "sts-policies";

    StsPolicyStore writer{file};
    Check(writer.load(At(100)).has_value(), "missing policy file loads as empty");
    Check(writer.apply_verified_update("IRC.Example.", 7000, true, 1, Persist(3600, true), At(100)).has_value(),
          "verified secure persistence update commits");
    Check(std::filesystem::is_regular_file(file), "persistence update creates policy file");
#if !defined(_WIN32)
    const auto permissions = std::filesystem::status(file).permissions();
    constexpr auto exposed_write = std::filesystem::perms::group_write |
                                   std::filesystem::perms::others_write;
    Check((permissions & exposed_write) == std::filesystem::perms::none,
          "durable policy is not group- or world-writable");
#endif

    StsPolicyStore restarted{file};
    Check(restarted.load(At(200)).has_value(), "fresh process loads durable policy");
    const auto first = restarted.plan(PlaintextRequest(), At(200));
    Check(first && first->enforced && first->options.security == Security::tls &&
              first->options.endpoint.port == 7000,
          "active hostname policy upgrades plaintext before transport start");
    const auto retry = restarted.plan(PlaintextRequest(), At(201));
    Check(retry && retry->enforced && retry->options.security == Security::tls &&
              retry->options.endpoint.port == 7000,
          "retry after TLS failure cannot downgrade to plaintext");
    const auto other = restarted.plan(PlaintextRequest("chat.example"), At(201));
    Check(other && !other->enforced && other->options.security == Security::plaintext,
          "policy is keyed to the requested hostname");

    StsPolicyStore expired{file};
    Check(expired.load(At(3700)).has_value(), "expired store still parses");
    const auto expired_plan = expired.plan(PlaintextRequest(), At(3700));
    Check(expired_plan && !expired_plan->enforced && expired_plan->options.security == Security::plaintext,
          "policy stops enforcing at its exact expiry");
}

void TestSecureDurationZeroRemovalPersists() {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    StsPolicyStore store{file};
    Check(store.load(At(10)).has_value(), "empty removal test store loads");
    Check(store.apply_verified_update("irc.example", 6697, true, 1, Persist(600), At(10)).has_value(),
          "initial policy persists");
    Check(store.apply_verified_update("irc.example", 6697, true, 1, Remove(), At(20)).has_value(),
          "verified duration zero removal commits");

    StsPolicyStore restarted{file};
    Check(restarted.load(At(21)).has_value(), "removed policy file reloads");
    const auto plan = restarted.plan(PlaintextRequest(), At(21));
    Check(plan && !plan->enforced && plan->options.security == Security::plaintext,
          "duration zero removal survives restart");
}

void TestInterleavedStoresMergeHostPersistence() {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    StsPolicyStore first{file};
    StsPolicyStore second{file};
    Check(first.load(At(10)).has_value() && second.load(At(10)).has_value(),
          "interleaved persistence stores load the same empty snapshot");
    Check(first.apply_verified_update("a.example", 7001, true, 1, Persist(1000), At(20)).has_value(),
          "first store persists host A");
    Check(second.apply_verified_update("b.example", 7002, true, 2, Persist(1000), At(21)).has_value(),
          "stale second store merges host B");

    const auto published_a = second.plan(PlaintextRequest("a.example"), At(22));
    Check(published_a && published_a->enforced && published_a->options.endpoint.port == 7001,
          "second store publishes the merged host A snapshot in memory");
    StsPolicyStore restarted{file};
    Check(restarted.load(At(22)).has_value(), "interleaved persistence result reloads");
    const auto durable_a = restarted.plan(PlaintextRequest("a.example"), At(22));
    const auto durable_b = restarted.plan(PlaintextRequest("b.example"), At(22));
    Check(durable_a && durable_a->enforced && durable_a->options.endpoint.port == 7001,
          "host A survives a stale store persisting host B");
    Check(durable_b && durable_b->enforced && durable_b->options.endpoint.port == 7002,
          "host B persistence remains durable after the merge");
}

void TestInterleavedStoresMergeHostRemoval() {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    StsPolicyStore second{file};
    Check(second.load(At(10)).has_value(), "host B removal store loads");
    Check(second.apply_verified_update("b.example", 7002, true, 2, Persist(1000), At(10)).has_value(),
          "host B removal baseline persists");
    StsPolicyStore first{file};
    Check(first.load(At(20)).has_value(), "host A writer loads the host B snapshot");
    Check(first.apply_verified_update("a.example", 7001, true, 1, Persist(1000), At(21)).has_value(),
          "host A persists after the removal store becomes stale");
    Check(second.apply_verified_update("b.example", 7002, true, 2, Remove(), At(22)).has_value(),
          "stale second store removes only host B");

    const auto published_a = second.plan(PlaintextRequest("a.example"), At(23));
    Check(published_a && published_a->enforced && published_a->options.endpoint.port == 7001,
          "removing host B publishes merged host A in memory");
    StsPolicyStore restarted{file};
    Check(restarted.load(At(23)).has_value(), "interleaved removal result reloads");
    const auto durable_a = restarted.plan(PlaintextRequest("a.example"), At(23));
    const auto removed_b = restarted.plan(PlaintextRequest("b.example"), At(23));
    Check(durable_a && durable_a->enforced && durable_a->options.endpoint.port == 7001,
          "host A survives a stale store removing host B");
    Check(removed_b && !removed_b->enforced, "host B removal remains durable after the merge");
}

void TestInterleavedStoresMergeHostReschedule() {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    StsPolicyStore second{file};
    Check(second.load(At(100)).has_value(), "host B reschedule store loads");
    const auto receipt = second.apply_verified_update("b.example", 7002, true, 2, Persist(100), At(100));
    Check(receipt && *receipt, "host B reschedule baseline returns a receipt");
    if (!receipt || !*receipt) return;

    StsPolicyStore first{file};
    Check(first.load(At(101)).has_value(), "host A writer loads the host B snapshot");
    Check(first.apply_verified_update("a.example", 7001, true, 1, Persist(1000), At(101)).has_value(),
          "host A persists after the reschedule store becomes stale");
    Check(second.reschedule_on_verified_disconnect("b.example", true, 2, *receipt, At(500)).has_value(),
          "stale second store rebases only host B after its prior expiry");

    const auto published_a = second.plan(PlaintextRequest("a.example"), At(550));
    Check(published_a && published_a->enforced && published_a->options.endpoint.port == 7001,
          "rescheduling host B publishes merged host A in memory");
    StsPolicyStore restarted{file};
    Check(restarted.load(At(550)).has_value(), "interleaved reschedule result reloads");
    const auto durable_a = restarted.plan(PlaintextRequest("a.example"), At(550));
    const auto rebased_b = restarted.plan(PlaintextRequest("b.example"), At(599));
    Check(durable_a && durable_a->enforced && durable_a->options.endpoint.port == 7001,
          "host A survives a stale store rescheduling host B");
    Check(rebased_b && rebased_b->enforced && rebased_b->options.endpoint.port == 7002,
          "host B remains active until its rebased expiry");
}

void TestOnlyVerifiedSecureUpdatesPersist() {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    StsPolicyStore store{file};
    Check(store.load(At(10)).has_value(), "verification test store loads");
    const auto insecure = store.apply_verified_update("irc.example", 6697, false, 1, Persist(600), At(10));
    Check(!insecure && insecure.error() == StsStoreError::invalid_update,
          "plaintext observation cannot establish persistence");
    const StsPolicyUpdate upgrade{StsPolicyAction::Upgrade, 6697, 0, false};
    const auto wrong_action = store.apply_verified_update("irc.example", 6697, true, 1, upgrade, At(10));
    Check(!wrong_action && wrong_action.error() == StsStoreError::invalid_update,
          "upgrade instruction cannot enter durable store");
    Check(store.size() == 0 && !std::filesystem::exists(file),
          "rejected updates do not create durable state");
}

void TestAtomicBoundedParsingPreservesPriorSnapshot() {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    StsPolicyStore store{file};
    Check(store.load(At(10)).has_value(), "atomic parse store loads");
    Check(store.apply_verified_update("irc.example", 6697, true, 1, Persist(600), At(10)).has_value(),
          "atomic parse baseline policy persists");

    {
        std::ofstream corrupt{file, std::ios::binary | std::ios::trunc};
        corrupt << "comicchat-sts-v1\nnot-a-policy\n";
    }
    const auto malformed = store.load(At(20));
    Check(!malformed && malformed.error() == StsStoreError::malformed_file,
          "malformed replacement is rejected atomically");
    const auto retained = store.plan(PlaintextRequest(), At(20));
    Check(retained && retained->enforced,
          "failed load leaves the previous in-memory policy snapshot intact");

    {
        std::ofstream oversized{file, std::ios::binary | std::ios::trunc};
        std::string block(4096, 'x');
        for (std::size_t written = 0; written <= StsPolicyStore::maximum_file_bytes; written += block.size()) {
            oversized.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }
    const auto too_large = store.load(At(20));
    Check(!too_large && too_large.error() == StsStoreError::file_too_large,
          "oversized policy file is rejected before parsing");
}

void TestMutationReloadAndLockFailuresFailClosed() {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    StsPolicyStore store{file};
    Check(store.load(At(10)).has_value(), "mutation reload test store loads");
    Check(store.apply_verified_update("a.example", 7001, true, 1, Persist(600), At(10)).has_value(),
          "mutation reload baseline persists");
    {
        std::ofstream corrupt{file, std::ios::binary | std::ios::trunc};
        corrupt << "comicchat-sts-v1\nnot-a-policy\n";
    }
    const auto rejected = store.apply_verified_update("b.example", 7002, true, 2, Persist(600), At(20));
    Check(!rejected && rejected.error() == StsStoreError::malformed_file,
          "mutation rejects a malformed durable snapshot instead of overwriting it");
    const auto retained = store.plan(PlaintextRequest("a.example"), At(20));
    Check(retained && retained->enforced && retained->options.endpoint.port == 7001,
          "failed transaction reload preserves the prior in-memory snapshot");
    StsPolicyStore malformed{file};
    const auto still_malformed = malformed.load(At(20));
    Check(!still_malformed && still_malformed.error() == StsStoreError::malformed_file,
          "failed transaction reload leaves malformed durable bytes untouched");

    const auto locked_file = temporary.path / "locked-policies";
    StsPolicyStore blocked{locked_file};
    Check(blocked.load(At(10)).has_value(), "lock failure test store loads");
    auto lock_path = locked_file;
    lock_path += ".lock";
    std::error_code error;
    Check(std::filesystem::create_directory(lock_path, error) && !error,
          "invalid lock-path fixture is created");
    const auto lock_failed = blocked.apply_verified_update(
        "a.example", 7001, true, 1, Persist(600), At(10));
    Check(!lock_failed && (lock_failed.error() == StsStoreError::io_error ||
                           lock_failed.error() == StsStoreError::unsafe_file),
          "lock acquisition failure rejects the mutation");
    Check(blocked.size() == 0 && !std::filesystem::exists(locked_file),
          "lock acquisition failure changes neither memory nor durable state");
}

void TestCrashResidueAndConcurrentTemporaryCollisions() {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    std::error_code error;
    std::filesystem::create_directory(file.string() + ".tmp", error);
    StsPolicyStore residue{file};
    Check(residue.load(At(10)).has_value(), "crash-residue store loads");
    Check(residue.apply_verified_update("irc.example", 6697, true, 1, Persist(600), At(10)).has_value(),
          "stale fixed-name temporary residue cannot permanently deny persistence");
    std::filesystem::remove_all(file.string() + ".tmp", error);

    constexpr std::size_t writers = 8;
    std::barrier rendezvous{static_cast<std::ptrdiff_t>(writers)};
    std::array<std::atomic_bool, writers> succeeded{};
    std::vector<std::thread> threads;
    threads.reserve(writers);
    for (std::size_t index = 0; index < writers; ++index) {
        threads.emplace_back([&, index] {
            StsPolicyStore store{file};
            const auto loaded = store.load(At(20));
            rendezvous.arrive_and_wait();
            if (!loaded) return;
            const auto hostname = "writer" + std::to_string(index) + ".example";
            const auto updated = store.apply_verified_update(
                hostname, static_cast<std::uint16_t>(7000 + index), true,
                static_cast<comicchat::net::GenerationId>(index + 1), Persist(1200), At(20));
            succeeded[index].store(updated.has_value(), std::memory_order_relaxed);
        });
    }
    for (auto& thread : threads) thread.join();
    Check(std::ranges::all_of(succeeded, [](const std::atomic_bool& value) {
              return value.load(std::memory_order_relaxed);
          }),
          "parallel writers complete locked merge transactions");

    StsPolicyStore restarted{file};
    Check(restarted.load(At(21)).has_value(), "concurrent atomic replacements leave one complete file");
    Check(restarted.size() == writers + 1U,
          "parallel transactions preserve the baseline and every distinct hostname");
    for (std::size_t index = 0; index < writers; ++index) {
        const auto hostname = "writer" + std::to_string(index) + ".example";
        const auto durable = restarted.plan(PlaintextRequest(hostname), At(21));
        Check(durable && durable->enforced &&
                  durable->options.endpoint.port == static_cast<std::uint16_t>(7000 + index),
              "each parallel hostname remains enforceable after locked merging");
    }
    const auto prefix = file.filename().string() + ".tmp.";
    bool leaked_temporary{};
    for (const auto& entry : std::filesystem::directory_iterator(temporary.path)) {
        if (entry.path().filename().string().starts_with(prefix)) leaked_temporary = true;
    }
    Check(!leaked_temporary, "successful replacements clean every unique temporary file");
}

void TestDisconnectReschedulesLastVerifiedDuration() {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    StsPolicyStore store{file};
    Check(store.load(At(100)).has_value(), "reschedule store loads");
    const auto persisted = store.apply_verified_update("irc.example", 7443, true, 42, Persist(100), At(100));
    Check(persisted && *persisted,
          "short verified policy persists and returns a connection-scoped receipt");
    const auto stale_generation = store.reschedule_on_verified_disconnect(
        "irc.example", true, 43, *persisted, At(400));
    Check(!stale_generation && stale_generation.error() == StsStoreError::invalid_update,
          "receipt from another connection generation cannot extend expiry");
    Check(store.reschedule_on_verified_disconnect("irc.example", true, 42, *persisted, At(500)).has_value(),
          "disconnect rebases the last advertised duration");

    StsPolicyStore restarted{file};
    Check(restarted.load(At(550)).has_value(), "rescheduled policy reloads");
    const auto active = restarted.plan(PlaintextRequest(), At(599));
    Check(active && active->enforced && active->options.endpoint.port == 7443,
          "rescheduled policy remains active until rebased expiry");
    const auto expired = restarted.plan(PlaintextRequest(), At(600));
    Check(expired && !expired->enforced,
          "rescheduled policy expires at the rebased boundary");
    const auto insecure = store.reschedule_on_verified_disconnect("irc.example", false, 42, *persisted, At(700));
    Check(!insecure && insecure.error() == StsStoreError::invalid_update,
          "plaintext disconnect cannot extend a policy");
}

void TestDisconnectWithoutSameSessionAdvertisementDoesNotExtend() {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    StsPolicyStore store{file};
    Check(store.load(At(100)).has_value(), "no-advertisement store loads");
    Check(store.apply_verified_update("irc.example", 6697, true, 1, Persist(100), At(100)).has_value(),
          "older cached policy persists");
    const std::optional<comicchat::net::StsPolicyReceipt> no_advertisement;
    Check(store.reschedule_on_verified_disconnect(
              "irc.example", true, 2, no_advertisement, At(150)).has_value(),
          "secure session without STS leaves the older expiry untouched");

    StsPolicyStore restarted{file};
    Check(restarted.load(At(200)).has_value(), "non-extended policy file remains parseable");
    const auto expired = restarted.plan(PlaintextRequest(), At(200));
    Check(expired && !expired->enforced,
          "cached policy is not perpetuated by a later secure session with no STS advertisement");
}

void TestBoundsAndUnsafeFilesFailClosed() {
    TemporaryDirectory temporary;
    const auto file = temporary.path / "sts-policies";
    StsPolicyStore store{file};
    const auto before_load = store.plan(PlaintextRequest(), At(10));
    Check(!before_load && before_load.error() == StsStoreError::not_loaded,
          "policy lookup cannot silently proceed before durable state is loaded");
    Check(store.load(At(10)).has_value(), "bounds test store loads");
    const auto invalid_hostname = store.plan(
        PlaintextRequest(std::string(StsPolicyStore::maximum_hostname_bytes + 1U, 'a')), At(10));
    Check(!invalid_hostname && invalid_hostname.error() == StsStoreError::invalid_hostname,
          "hostname key length is bounded before lookup");
    const auto overflowing = store.apply_verified_update(
        "irc.example", 6697, true, 1, Persist((std::numeric_limits<std::uint64_t>::max)()), At(10));
    Check(!overflowing && overflowing.error() == StsStoreError::clock_overflow,
          "duration arithmetic rejects expiry overflow");

    {
        std::ofstream many{file, std::ios::binary | std::ios::trunc};
        many << "comicchat-sts-v1\n";
        for (std::size_t index = 0; index <= StsPolicyStore::maximum_policies; ++index) {
            const auto hostname = "h" + std::to_string(index) + ".example";
            many << Hex(hostname) << "\t6697\t100\t1000\t0\n";
        }
    }
    StsPolicyStore bounded{file};
    const auto too_many = bounded.load(At(10));
    Check(!too_many && too_many.error() == StsStoreError::too_many_policies,
          "policy entry count is bounded during all-or-nothing parsing");

#if !defined(_WIN32)
    const auto real_file = temporary.path / "real-policies";
    StsPolicyStore real{real_file};
    Check(real.load(At(10)).has_value(), "symlink target store loads");
    Check(real.apply_verified_update("irc.example", 6697, true, 1, Persist(100), At(10)).has_value(),
          "symlink target policy persists");
    const auto link = temporary.path / "linked-policies";
    std::error_code error;
    std::filesystem::create_symlink(real_file, link, error);
    Check(!error, "policy symlink fixture created");
    if (!error) {
        StsPolicyStore linked{link};
        const auto unsafe = linked.load(At(10));
        Check(!unsafe && unsafe.error() == StsStoreError::unsafe_file,
              "policy loading rejects symlink substitution");
    }

    const auto locked_file = temporary.path / "lock-symlink-policies";
    StsPolicyStore lock_store{locked_file};
    Check(lock_store.load(At(10)).has_value(), "lock symlink test store loads");
    auto lock_link = locked_file;
    lock_link += ".lock";
    error.clear();
    std::filesystem::create_symlink(real_file, lock_link, error);
    Check(!error, "lock-file symlink fixture created");
    if (!error) {
        const auto unsafe = lock_store.apply_verified_update(
            "irc.example", 6697, true, 1, Persist(100), At(10));
        Check(!unsafe && unsafe.error() == StsStoreError::unsafe_file,
              "policy mutation rejects lock-file symlink substitution");
        Check(!std::filesystem::exists(locked_file),
              "rejected lock-file symlink leaves durable policy state untouched");
    }
#endif
}

} // namespace

int main() {
    TestDurableHostnamePolicyAndNoDowngrade();
    TestSecureDurationZeroRemovalPersists();
    TestInterleavedStoresMergeHostPersistence();
    TestInterleavedStoresMergeHostRemoval();
    TestInterleavedStoresMergeHostReschedule();
    TestOnlyVerifiedSecureUpdatesPersist();
    TestAtomicBoundedParsingPreservesPriorSnapshot();
    TestMutationReloadAndLockFailuresFailClosed();
    TestCrashResidueAndConcurrentTemporaryCollisions();
    TestDisconnectReschedulesLastVerifiedDuration();
    TestDisconnectWithoutSameSessionAdvertisementDoesNotExtend();
    TestBoundsAndUnsafeFilesFailClosed();
    if (failures != 0) {
        std::cerr << failures << " STS policy test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "STS policy tests passed\n";
    return EXIT_SUCCESS;
}
