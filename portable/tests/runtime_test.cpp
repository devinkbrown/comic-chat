#include "comicchat/net/connection_engine.hpp"
#include "comicchat/memory.hpp"
#include "comicchat/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <catch2/catch_test_macros.hpp>

using namespace std::string_literals;
using namespace std::chrono_literals;

TEST_CASE("connection generations reject stale commands") {
    comicchat::net::ConnectionEngine engine;
    comicchat::net::ConnectionOptions options;
    options.endpoint = {"example.invalid", 6697};
    const auto generation = engine.start(options);
    REQUIRE(generation.has_value());
    CHECK_FALSE(engine.post(comicchat::net::Disconnect{*generation - 1, "stale"}).has_value());
    CHECK(engine.post(comicchat::net::Disconnect{*generation, "test"}).has_value());
    engine.stop();
}

TEST_CASE("connection options reject control-character injection") {
    const auto rejected = [](comicchat::net::ConnectionOptions options) {
        comicchat::net::ConnectionEngine engine;
        const auto result = engine.start(std::move(options));
        return !result && result.error() == comicchat::net::EngineError::invalid_options;
    };

    comicchat::net::ConnectionOptions endpoint;
    endpoint.endpoint = {"irc.example\r\nInjected: yes", 6697};
    CHECK(rejected(endpoint));

    comicchat::net::ConnectionOptions ambiguous_authority;
    ambiguous_authority.endpoint = {"irc.example:6697", 6697};
    CHECK(rejected(ambiguous_authority));

    comicchat::net::ConnectionOptions bracketed_address;
    bracketed_address.endpoint = {"[::1]", 6697};
    CHECK(rejected(bracketed_address));

    comicchat::net::ConnectionOptions server_name;
    server_name.endpoint = {"irc.example", 6697};
    server_name.server_name = "irc.example\0wrong"s;
    CHECK(rejected(server_name));

    comicchat::net::ConnectionOptions proxy;
    proxy.endpoint = {"irc.example", 6697};
    proxy.proxy.kind = comicchat::net::ProxyKind::http_connect;
    proxy.proxy.host = "proxy.example\nBad";
    proxy.proxy.port = 8080;
    CHECK(rejected(proxy));

    comicchat::net::ConnectionOptions ca_path;
    ca_path.endpoint = {"irc.example", 6697};
    ca_path.ca_file = "ca.pem\0ignored"s;
    CHECK(rejected(ca_path));
}

TEST_CASE("proxy credentials fail explicitly when native page locking is unavailable") {
    comicchat::net::ConnectionOptions options;
    options.endpoint = {"irc.example", 6697};
    options.proxy.kind = comicchat::net::ProxyKind::http_connect;
    options.proxy.host = "proxy.example";
    options.proxy.port = 8080;
    options.proxy.username = "alice";
    options.proxy.password = "secret";
    comicchat::testing::fail_next_secret_lock();
    comicchat::net::ConnectionEngine engine;
    const auto result = engine.start(std::move(options));
    REQUIRE_FALSE(result);
    CHECK(result.error() == comicchat::net::EngineError::credential_lock_failed);
}

TEST_CASE("deterministic scheduler runs inline and drops stale generations") {
    comicchat::WorkerScheduler scheduler{1, 2, true};
    scheduler.advance_generation(7);
    std::atomic_uint count{};
    auto future = scheduler.submit(7, [&](comicchat::threading::StopToken) { ++count; });
    REQUIRE(future.has_value());
    future->get();
    CHECK(count == 1);
    CHECK_FALSE(scheduler.submit(6, [](comicchat::threading::StopToken) {}).has_value());
    scheduler.stop();
}

TEST_CASE("deterministic scheduler captures task failures in the future") {
    comicchat::WorkerScheduler scheduler{1, 2, true};
    scheduler.advance_generation(1);
    auto future = scheduler.submit(1, [](comicchat::threading::StopToken) { throw std::runtime_error{"task failure"}; });
    REQUIRE(future);
    CHECK_THROWS_AS(future->get(), std::runtime_error);
    scheduler.stop();
}

TEST_CASE("generation changes stop in-flight work and gate its stale result") {
    comicchat::WorkerScheduler scheduler{1, 4, false};
    scheduler.advance_generation(7);
    std::atomic_bool started{};
    std::atomic_bool release{};
    std::atomic_bool saw_stop{};
    auto future = scheduler.submit(7, [&](const comicchat::threading::StopToken token) {
        started.store(true);
        while (!release.load()) {
            saw_stop.store(token.stop_requested());
            std::this_thread::yield();
        }
    });
    REQUIRE(future);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!started.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(started.load());
    scheduler.advance_generation(8);
    const auto stop_deadline = std::chrono::steady_clock::now() + 1s;
    while (!saw_stop.load() && std::chrono::steady_clock::now() < stop_deadline) std::this_thread::yield();
    release.store(true);
    CHECK_THROWS_AS(future->get(), std::future_error);
    CHECK(saw_stop.load());
    scheduler.stop();
}

TEST_CASE("scheduler stop completes queued futures without allocating cancellation exceptions") {
    comicchat::WorkerScheduler scheduler{1, 4, false};
    scheduler.advance_generation(3);
    std::atomic_bool started{};
    std::atomic_bool release{};
    auto running = scheduler.submit(3, [&](comicchat::threading::StopToken) {
        started.store(true);
        while (!release.load()) std::this_thread::yield();
    });
    REQUIRE(running);
    auto queued = scheduler.submit(3, [](comicchat::threading::StopToken) {});
    REQUIRE(queued);
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!started.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(started.load());
    comicchat::threading::JThread releaser{[&] {
        std::this_thread::sleep_for(10ms);
        release.store(true);
    }};
    scheduler.stop();
    CHECK_THROWS_AS(queued->get(), std::future_error);
}
