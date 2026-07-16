#include "comicchat/config.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("TLS and port 6697 are secure defaults") {
    constexpr std::array<std::string_view, 3> args{"irc.example", "nick", "#room"};
    const auto config = comicchat::parse_connection_args(args);
    REQUIRE(config.has_value());
    CHECK(config->port == 6697);
    CHECK(config->security == comicchat::Security::tls);
}

TEST_CASE("plaintext requires an explicit compatibility option") {
    constexpr std::array<std::string_view, 6> args{
        "localhost", "6667", "nick", "#room", "--plaintext", "--ca-file=ignored.pem"};
    const auto config = comicchat::parse_connection_args(args);
    REQUIRE(config.has_value());
    CHECK(config->port == 6667);
    CHECK(config->security == comicchat::Security::plaintext);
    CHECK(config->ca_file == "ignored.pem");
}
