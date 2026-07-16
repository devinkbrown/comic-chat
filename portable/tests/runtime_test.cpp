#include "comicchat/net/connection_engine.hpp"
#include "comicchat/scheduler.hpp"

#include <atomic>
#include <string>
#include <utility>
#include <catch2/catch_test_macros.hpp>

using namespace std::string_literals;

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

TEST_CASE("deterministic scheduler runs inline and drops stale generations") {
    comicchat::WorkerScheduler scheduler{1, 2, true};
    scheduler.advance_generation(7);
    std::atomic_uint count{};
    auto future = scheduler.submit(7, [&](std::stop_token) { ++count; });
    REQUIRE(future.has_value());
    future->get();
    CHECK(count == 1);
    CHECK_FALSE(scheduler.submit(6, [](std::stop_token) {}).has_value());
    scheduler.stop();
}
