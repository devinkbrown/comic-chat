#include "comicchat/net/connection_engine.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/aes.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cache.h>
#include <mbedtls/x509_crt.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <psa/crypto.h>

using namespace std::chrono_literals;

namespace {

[[maybe_unused]] const bool sigpipe_ignored = [] {
    std::signal(SIGPIPE, SIG_IGN);
    return true;
}();

void warm_mbedtls_cpu_features() {
    static const bool initialized = [] {
        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        std::array<unsigned char, 16> key{};
        const auto result = mbedtls_ssl_list_ciphersuites() != nullptr &&
            psa_crypto_init() == PSA_SUCCESS &&
            mbedtls_aes_setkey_enc(&aes, key.data(), 128) == 0;
        mbedtls_aes_free(&aes);
        return result;
    }();
    REQUIRE(initialized);
}

enum class ServerMode { plaintext_echo, plaintext_burst, tls_echo, tls_stall };
enum class ProxyMode { socks5, http_connect };

class LoopbackServer final {
public:
    explicit LoopbackServer(const ServerMode mode) : mode_{mode} {
        warm_mbedtls_cpu_features();
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener_ >= 0);
        const int reuse = 1;
        REQUIRE(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listener_, 4) == 0);
        socklen_t size = sizeof(address);
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread{[this](const std::stop_token token) { run(token); }};
    }

    ~LoopbackServer() {
        thread_.request_stop();
        if (const auto client = client_.load(); client >= 0) (void)::shutdown(client, SHUT_RDWR);
        if (listener_ >= 0) (void)::shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (const auto client = client_.exchange(-1); client >= 0) (void)::close(client);
        if (listener_ >= 0) (void)::close(listener_);
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t { return port_; }

private:
    void run(const std::stop_token token) {
        sockaddr_in peer{};
        socklen_t size = sizeof(peer);
        const auto accepted = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &size);
        if (accepted < 0 || token.stop_requested()) return;
        client_.store(accepted);
        if (mode_ == ServerMode::plaintext_echo) {
            std::array<std::byte, 256> buffer{};
            const auto count = ::recv(accepted, buffer.data(), buffer.size(), 0);
            if (count > 0) (void)::send(accepted, buffer.data(), static_cast<std::size_t>(count), 0);
            return;
        }
        if (mode_ == ServerMode::plaintext_burst) {
            std::array<std::byte, 24> output{};
            for (std::size_t index = 0; index < output.size(); ++index)
                output[index] = std::byte{static_cast<unsigned char>(index)};
            (void)::send(accepted, output.data(), output.size(), 0);
            while (!token.stop_requested()) std::this_thread::sleep_for(5ms);
            return;
        }
        if (mode_ == ServerMode::tls_stall) {
            while (!token.stop_requested()) std::this_thread::sleep_for(5ms);
            return;
        }
        run_tls_echo(accepted);
    }

    static void run_tls_echo(const int accepted) {
        mbedtls_ssl_context ssl;
        mbedtls_ssl_config config;
        mbedtls_x509_crt certificate;
        mbedtls_pk_context key;
        mbedtls_ctr_drbg_context random;
        mbedtls_entropy_context entropy;
        mbedtls_net_context client;
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&certificate);
        mbedtls_pk_init(&key);
        mbedtls_ctr_drbg_init(&random);
        mbedtls_entropy_init(&entropy);
        mbedtls_net_init(&client);
        client.fd = accepted;
        static constexpr unsigned char personalization[] = "comic-chat-loopback";
        const std::string certificate_path = std::string{MBEDTLS_DATA_DIR} + "/server6.crt";
        const std::string key_path = std::string{MBEDTLS_DATA_DIR} + "/server6.key";
        const auto initialized =
            mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy, personalization,
                                  sizeof(personalization) - 1) == 0 &&
            mbedtls_x509_crt_parse_file(&certificate, certificate_path.c_str()) == 0 &&
            mbedtls_pk_parse_keyfile(&key, key_path.c_str(), nullptr,
                                     mbedtls_ctr_drbg_random, &random) == 0 &&
            mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0 &&
            mbedtls_ssl_conf_own_cert(&config, &certificate, &key) == 0;
        if (initialized) {
            mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
            if (mbedtls_ssl_setup(&ssl, &config) == 0) {
                mbedtls_ssl_set_bio(&ssl, &client, mbedtls_net_send, mbedtls_net_recv, nullptr);
                if (mbedtls_ssl_handshake(&ssl) == 0) {
                    std::array<unsigned char, 256> buffer{};
                    const auto count = mbedtls_ssl_read(&ssl, buffer.data(), buffer.size());
                    if (count > 0) (void)mbedtls_ssl_write(&ssl, buffer.data(), static_cast<std::size_t>(count));
                    (void)mbedtls_ssl_close_notify(&ssl);
                }
            }
        }
        client.fd = -1;
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&certificate);
        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&random);
        mbedtls_entropy_free(&entropy);
    }

    ServerMode mode_{};
    int listener_{-1};
    std::atomic_int client_{-1};
    std::uint16_t port_{};
    std::jthread thread_;
};

class ProxyServer final {
public:
    explicit ProxyServer(const ProxyMode mode, const bool coalesced_banner = false)
        : mode_{mode}, coalesced_banner_{coalesced_banner} {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener_ >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listener_, 1) == 0);
        socklen_t size = sizeof(address);
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread{[this](const std::stop_token token) { run(token); }};
    }

    ~ProxyServer() {
        thread_.request_stop();
        if (const auto client = client_.load(); client >= 0) (void)::shutdown(client, SHUT_RDWR);
        if (listener_ >= 0) (void)::shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (const auto client = client_.exchange(-1); client >= 0) (void)::close(client);
        if (listener_ >= 0) (void)::close(listener_);
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t { return port_; }
    [[nodiscard]] auto request() const -> std::string {
        std::scoped_lock lock{request_mutex_};
        return request_;
    }

private:
    static auto read_exact(const int socket, std::span<std::byte> output) -> bool {
        std::size_t offset{};
        while (offset < output.size()) {
            const auto count = ::recv(socket, output.data() + offset, output.size() - offset, 0);
            if (count <= 0) return false;
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    void run(const std::stop_token token) {
        sockaddr_in peer{};
        socklen_t size = sizeof(peer);
        const auto accepted = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &size);
        if (accepted < 0 || token.stop_requested()) return;
        client_.store(accepted);
        if (mode_ == ProxyMode::socks5) {
            std::array<std::byte, 3> greeting{};
            if (!read_exact(accepted, greeting)) return;
            constexpr std::array<std::byte, 2> greeting_reply{std::byte{5}, std::byte{0}};
            (void)::send(accepted, greeting_reply.data(), greeting_reply.size(), 0);
            std::array<std::byte, 5> request{};
            if (!read_exact(accepted, request)) return;
            const auto name_size = std::to_integer<unsigned char>(request[4]);
            std::vector<std::byte> target(static_cast<std::size_t>(name_size) + 2);
            if (!read_exact(accepted, target)) return;
            constexpr std::array<std::byte, 10> connected{
                std::byte{5}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{127},
                std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0}, std::byte{1},
            };
            std::vector<std::byte> reply{connected.begin(), connected.end()};
            if (coalesced_banner_) {
                constexpr std::string_view banner{"ready"};
                for (const auto character : banner)
                    reply.push_back(std::byte{static_cast<unsigned char>(character)});
            }
            (void)::send(accepted, reply.data(), reply.size(), 0);
        } else {
            std::string request;
            std::array<char, 256> input{};
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 4096) {
                const auto count = ::recv(accepted, input.data(), input.size(), 0);
                if (count <= 0) return;
                request.append(input.data(), static_cast<std::size_t>(count));
            }
            {
                std::scoped_lock lock{request_mutex_};
                request_ = request;
            }
            std::string connected = "HTTP/1.1 200 Connection Established\r\n\r\n";
            if (coalesced_banner_) connected += "ready";
            (void)::send(accepted, connected.data(), connected.size(), 0);
        }
        std::array<std::byte, 256> payload{};
        const auto count = ::recv(accepted, payload.data(), payload.size(), 0);
        if (count > 0) (void)::send(accepted, payload.data(), static_cast<std::size_t>(count), 0);
    }

    ProxyMode mode_{};
    bool coalesced_banner_{};
    int listener_{-1};
    std::atomic_int client_{-1};
    std::uint16_t port_{};
    mutable std::mutex request_mutex_;
    std::string request_;
    std::jthread thread_;
};

class ReconnectServer final {
public:
    ReconnectServer() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener_ >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listener_, 2) == 0);
        socklen_t size = sizeof(address);
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread{[this](const std::stop_token token) {
            for (int connection = 0; connection < 2 && !token.stop_requested(); ++connection) {
                const auto client = ::accept(listener_, nullptr, nullptr);
                if (client < 0) return;
                client_.store(client);
                if (connection == 0) {
                    (void)::shutdown(client, SHUT_RDWR);
                    (void)::close(client);
                    client_.store(-1);
                    continue;
                }
                std::array<std::byte, 64> input{};
                const auto count = ::recv(client, input.data(), input.size(), 0);
                if (count > 0) (void)::send(client, input.data(), static_cast<std::size_t>(count), 0);
            }
        }};
    }

    ~ReconnectServer() {
        thread_.request_stop();
        if (const auto client = client_.load(); client >= 0) (void)::shutdown(client, SHUT_RDWR);
        if (listener_ >= 0) (void)::shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (const auto client = client_.exchange(-1); client >= 0) (void)::close(client);
        if (listener_ >= 0) (void)::close(listener_);
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t { return port_; }

private:
    int listener_{-1};
    std::atomic_int client_{-1};
    std::uint16_t port_{};
    std::jthread thread_;
};

class TlsResumeServer final {
public:
    TlsResumeServer() {
        warm_mbedtls_cpu_features();
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listener_ >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        REQUIRE(::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listener_, 2) == 0);
        socklen_t size = sizeof(address);
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = std::jthread{[this](const std::stop_token token) { run(token); }};
    }

    ~TlsResumeServer() {
        thread_.request_stop();
        if (const auto client = client_.load(); client >= 0) (void)::shutdown(client, SHUT_RDWR);
        if (listener_ >= 0) (void)::shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (const auto client = client_.exchange(-1); client >= 0) (void)::close(client);
        if (listener_ >= 0) (void)::close(listener_);
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t { return port_; }

private:
    void run(const std::stop_token token) {
        mbedtls_ssl_config config;
        mbedtls_x509_crt certificate;
        mbedtls_pk_context key;
        mbedtls_ctr_drbg_context random;
        mbedtls_entropy_context entropy;
        mbedtls_ssl_cache_context cache;
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&certificate);
        mbedtls_pk_init(&key);
        mbedtls_ctr_drbg_init(&random);
        mbedtls_entropy_init(&entropy);
        mbedtls_ssl_cache_init(&cache);
        static constexpr unsigned char personalization[] = "comic-chat-resume";
        const std::string certificate_path = std::string{MBEDTLS_DATA_DIR} + "/server6.crt";
        const std::string key_path = std::string{MBEDTLS_DATA_DIR} + "/server6.key";
        const auto initialized =
            mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy, personalization,
                                  sizeof(personalization) - 1) == 0 &&
            mbedtls_x509_crt_parse_file(&certificate, certificate_path.c_str()) == 0 &&
            mbedtls_pk_parse_keyfile(&key, key_path.c_str(), nullptr,
                                     mbedtls_ctr_drbg_random, &random) == 0 &&
            mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER,
                                        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0 &&
            mbedtls_ssl_conf_own_cert(&config, &certificate, &key) == 0;
        if (initialized) {
            mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
            mbedtls_ssl_conf_max_tls_version(&config, MBEDTLS_SSL_VERSION_TLS1_2);
            mbedtls_ssl_conf_session_cache(&config, &cache, mbedtls_ssl_cache_get, mbedtls_ssl_cache_set);
            for (int connection = 0; connection < 2 && !token.stop_requested(); ++connection) {
                const auto accepted = ::accept(listener_, nullptr, nullptr);
                if (accepted < 0) break;
                client_.store(accepted);
                mbedtls_net_context client;
                mbedtls_net_init(&client);
                client.fd = accepted;
                mbedtls_ssl_context ssl;
                mbedtls_ssl_init(&ssl);
                if (mbedtls_ssl_setup(&ssl, &config) == 0) {
                    mbedtls_ssl_set_bio(&ssl, &client, mbedtls_net_send, mbedtls_net_recv, nullptr);
                    if (mbedtls_ssl_handshake(&ssl) == 0 && connection == 1)
                        while (!token.stop_requested()) std::this_thread::sleep_for(5ms);
                    (void)mbedtls_ssl_close_notify(&ssl);
                }
                client.fd = -1;
                mbedtls_ssl_free(&ssl);
                const auto current = client_.exchange(-1);
                if (current >= 0) {
                    (void)::shutdown(current, SHUT_RDWR);
                    (void)::close(current);
                }
            }
        }
        mbedtls_ssl_cache_free(&cache);
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&certificate);
        mbedtls_pk_free(&key);
        mbedtls_ctr_drbg_free(&random);
        mbedtls_entropy_free(&entropy);
    }

    int listener_{-1};
    std::atomic_int client_{-1};
    std::uint16_t port_{};
    std::jthread thread_;
};

auto wait_for(comicchat::net::ConnectionEngine& engine,
              const std::function<bool(const comicchat::net::Event&)>& predicate,
              const std::chrono::milliseconds timeout = 3s) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : engine.poll_events(1)) if (predicate(event)) return true;
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

auto options_for(const LoopbackServer& server, const comicchat::net::Security security)
    -> comicchat::net::ConnectionOptions {
    comicchat::net::ConnectionOptions options;
    options.endpoint = {"127.0.0.1", server.port()};
    options.security = security;
    options.server_name = "localhost";
    options.deadlines.connect = 1s;
    options.deadlines.handshake = 1s;
    options.deadlines.idle = 5s;
    if (security == comicchat::net::Security::tls)
        options.ca_file = std::string{MBEDTLS_DATA_DIR} + "/test-ca2.crt";
    return options;
}

auto bytes(std::string_view value) -> std::vector<std::byte> {
    std::vector<std::byte> result(value.size());
    std::memcpy(result.data(), value.data(), value.size());
    return result;
}

} // namespace

TEST_CASE("libuv plaintext loopback carries bounded bytes") {
    LoopbackServer server{ServerMode::plaintext_echo};
    comicchat::net::ConnectionEngine engine;
    const auto generation = engine.start(options_for(server, comicchat::net::Security::plaintext));
    REQUIRE(generation.has_value());
    REQUIRE(wait_for(engine, [](const auto& event) {
        const auto* connected = std::get_if<comicchat::net::Connected>(&event.body);
        return connected != nullptr && connected->local_address == "127.0.0.1";
    }));
    const auto before_idle = engine.stats();
    std::this_thread::sleep_for(100ms);
    const auto after_idle = engine.stats();
    CHECK(after_idle.loop_iterations - before_idle.loop_iterations <= 2);
    REQUIRE(engine.post(comicchat::net::Send{*generation, 7, comicchat::net::Priority::chat, bytes("hello"), false}).has_value());
    CHECK(wait_for(engine, [](const auto& event) {
        const auto* received = std::get_if<comicchat::net::BytesReceived>(&event.body);
        return received != nullptr && received->bytes && received->bytes->size() == 5;
    }));
    CHECK(engine.stats().command_wakeups > after_idle.command_wakeups);
    engine.stop();
}

TEST_CASE("receive backpressure pauses without dropping wire bytes") {
    LoopbackServer server{ServerMode::plaintext_burst};
    auto options = options_for(server, comicchat::net::Security::plaintext);
    options.limits.receive_bytes = 8;
    options.limits.queued_commands = 4;
    comicchat::net::ConnectionEngine engine;
    REQUIRE(engine.start(options));
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));
    std::size_t total{};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (total < 24 && std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : engine.poll_events(1)) {
            if (const auto* received = std::get_if<comicchat::net::BytesReceived>(&event.body);
                received != nullptr && received->bytes) {
                CHECK(received->bytes->size() <= 8);
                total += received->bytes->size();
            }
        }
        std::this_thread::sleep_for(2ms);
    }
    CHECK(total == 24);
    engine.stop();
}

TEST_CASE("mbedTLS loopback verifies localhost and carries encrypted bytes") {
    LoopbackServer server{ServerMode::tls_echo};
    comicchat::net::ConnectionEngine engine;
    const auto generation = engine.start(options_for(server, comicchat::net::Security::tls));
    REQUIRE(generation.has_value());
    REQUIRE(wait_for(engine, [](const auto& event) {
        const auto* connected = std::get_if<comicchat::net::Connected>(&event.body);
        return connected != nullptr && connected->tls;
    }));
    REQUIRE(engine.post(comicchat::net::Send{*generation, 9, comicchat::net::Priority::authentication,
                                             bytes("secret"), true}).has_value());
    CHECK(wait_for(engine, [](const auto& event) { return std::holds_alternative<comicchat::net::BytesReceived>(event.body); }));
    engine.stop();
}

TEST_CASE("mbedTLS hostname mismatch fails closed") {
    LoopbackServer server{ServerMode::tls_echo};
    auto options = options_for(server, comicchat::net::Security::tls);
    options.server_name = "wrong.invalid";
    comicchat::net::ConnectionEngine engine;
    REQUIRE(engine.start(options).has_value());
    CHECK(wait_for(engine, [](const auto& event) {
        const auto* diagnostic = std::get_if<comicchat::net::Diagnostic>(&event.body);
        return diagnostic != nullptr && diagnostic->code == "tls-handshake";
    }));
    CHECK_FALSE(wait_for(engine, [](const auto& event) { return std::holds_alternative<comicchat::net::Connected>(event.body); }, 50ms));
    engine.stop();
}

TEST_CASE("TLS handshake deadline and cancellation are bounded") {
    LoopbackServer server{ServerMode::tls_stall};
    auto options = options_for(server, comicchat::net::Security::tls);
    options.deadlines.handshake = 100ms;
    comicchat::net::ConnectionEngine engine;
    REQUIRE(engine.start(options).has_value());
    CHECK(wait_for(engine, [](const auto& event) {
        const auto* diagnostic = std::get_if<comicchat::net::Diagnostic>(&event.body);
        return diagnostic != nullptr && diagnostic->code == "handshake-timeout";
    }, 1s));
    const auto started = std::chrono::steady_clock::now();
    engine.stop();
    CHECK(std::chrono::steady_clock::now() - started < 500ms);
}

TEST_CASE("transmit backpressure rejects an oversized command before allocation growth") {
    comicchat::net::ConnectionEngine engine;
    comicchat::net::ConnectionOptions options;
    options.endpoint = {"example.invalid", 6697};
    options.limits.transmit_bytes = 8;
    const auto generation = engine.start(options);
    REQUIRE(generation.has_value());
    CHECK_FALSE(engine.post(comicchat::net::Send{*generation, 1, comicchat::net::Priority::chat,
                                                  std::vector<std::byte>(9), false}).has_value());
    engine.stop();
}

TEST_CASE("every rejected sensitive command is explicitly zeroized") {
    comicchat::net::ConnectionEngine engine;
    comicchat::net::ConnectionOptions options;
    options.endpoint = {"example.invalid", 6697};
    options.limits.transmit_bytes = 8;
    const auto generation = engine.start(options);
    REQUIRE(generation.has_value());

    CHECK(engine.post(comicchat::net::Send{*generation - 1, 31, comicchat::net::Priority::authentication,
                                            bytes("stale"), true}) ==
          std::unexpected{comicchat::net::EngineError::stale_generation});
    CHECK(engine.post(comicchat::net::Send{*generation, 32, comicchat::net::Priority::authentication,
                                            bytes("too-large"), true}) ==
          std::unexpected{comicchat::net::EngineError::queue_full});
    CHECK(engine.stats().rejected_sensitive_bytes_wiped == 14);

    engine.stop();
    CHECK(engine.post(comicchat::net::Send{*generation, 33, comicchat::net::Priority::authentication,
                                            bytes("stopped"), true}) ==
          std::unexpected{comicchat::net::EngineError::not_running});
    CHECK(engine.stats().rejected_sensitive_bytes_wiped == 21);
}

TEST_CASE("SOCKS5 and HTTP CONNECT proxies establish real bounded tunnels") {
    for (const auto mode : {ProxyMode::socks5, ProxyMode::http_connect}) {
        ProxyServer proxy{mode, true};
        comicchat::net::ConnectionOptions options;
        options.endpoint = {"irc.example", 6667};
        options.security = comicchat::net::Security::plaintext;
        options.proxy.kind = mode == ProxyMode::socks5 ? comicchat::net::ProxyKind::socks5
                                                       : comicchat::net::ProxyKind::http_connect;
        options.proxy.host = "127.0.0.1";
        options.proxy.port = proxy.port();
        options.deadlines.connect = 1s;
        options.deadlines.handshake = 1s;
        options.deadlines.idle = 5s;
        comicchat::net::ConnectionEngine engine;
        const auto generation = engine.start(options);
        REQUIRE(generation.has_value());
        REQUIRE(wait_for(engine, [](const auto& event) {
            return std::holds_alternative<comicchat::net::Connected>(event.body);
        }));
        REQUIRE(wait_for(engine, [](const auto& event) {
            const auto* received = std::get_if<comicchat::net::BytesReceived>(&event.body);
            return received != nullptr && received->bytes && received->bytes->size() == 5;
        }));
        REQUIRE(engine.post(comicchat::net::Send{*generation, 19, comicchat::net::Priority::chat,
                                                 bytes("proxied"), false}).has_value());
        CHECK(wait_for(engine, [](const auto& event) {
            return std::holds_alternative<comicchat::net::BytesReceived>(event.body);
        }));
        engine.stop();
    }
}

TEST_CASE("HTTP CONNECT brackets an IPv6 target authority") {
    ProxyServer proxy{ProxyMode::http_connect};
    comicchat::net::ConnectionOptions options;
    options.endpoint = {"::1", 6667};
    options.security = comicchat::net::Security::plaintext;
    options.proxy.kind = comicchat::net::ProxyKind::http_connect;
    options.proxy.host = "127.0.0.1";
    options.proxy.port = proxy.port();
    options.deadlines.connect = 1s;
    options.deadlines.handshake = 1s;
    options.deadlines.idle = 5s;

    comicchat::net::ConnectionEngine engine;
    REQUIRE(engine.start(options));
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));
    CHECK(proxy.request().starts_with(
        "CONNECT [::1]:6667 HTTP/1.1\r\nHost: [::1]:6667\r\n"));
    engine.stop();
}

TEST_CASE("reconnect jitter accepts a deterministic seed without generation synchronization") {
    const auto first_retry = [](const std::uint64_t seed) {
        comicchat::net::ConnectionOptions options;
        options.endpoint = {"127.0.0.1", 1};
        options.security = comicchat::net::Security::plaintext;
        options.deadlines.connect = 250ms;
        options.reconnect_jitter_seed = seed;
        comicchat::net::ConnectionEngine engine;
        REQUIRE(engine.start(options));
        std::chrono::milliseconds retry{};
        REQUIRE(wait_for(engine, [&retry](const auto& event) {
            if (const auto* closed = std::get_if<comicchat::net::Closed>(&event.body)) {
                retry = closed->retry_after;
                return true;
            }
            return false;
        }));
        engine.stop();
        return retry;
    };

    CHECK(first_retry(0x1234U) == first_retry(0x1234U));
    CHECK(first_retry(0x1234U) != first_retry(0x5678U));
}

TEST_CASE("peer closure reconnects with jitter without replaying chat") {
    ReconnectServer server;
    comicchat::net::ConnectionOptions options;
    options.endpoint = {"127.0.0.1", server.port()};
    options.security = comicchat::net::Security::plaintext;
    options.deadlines.connect = 1s;
    options.deadlines.idle = 5s;
    comicchat::net::ConnectionEngine engine;
    const auto generation = engine.start(options);
    REQUIRE(generation.has_value());
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }, 3s));
    REQUIRE(engine.post(comicchat::net::Send{*generation, 23, comicchat::net::Priority::chat,
                                             bytes("after-reconnect"), false}).has_value());
    CHECK(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::BytesReceived>(event.body);
    }));
    engine.stop();
}

TEST_CASE("TLS reconnect reports actual cached-session resumption") {
    TlsResumeServer server;
    comicchat::net::ConnectionOptions options;
    options.endpoint = {"127.0.0.1", server.port()};
    options.security = comicchat::net::Security::tls;
    options.server_name = "localhost";
    options.ca_file = std::string{MBEDTLS_DATA_DIR} + "/test-ca2.crt";
    options.deadlines.connect = 1s;
    options.deadlines.handshake = 1s;
    options.deadlines.idle = 5s;
    comicchat::net::ConnectionEngine engine;
    REQUIRE(engine.start(options).has_value());
    REQUIRE(wait_for(engine, [](const auto& event) {
        const auto* connected = std::get_if<comicchat::net::Connected>(&event.body);
        return connected != nullptr && connected->tls && !connected->resumed;
    }));
    CHECK(wait_for(engine, [](const auto& event) {
        const auto* connected = std::get_if<comicchat::net::Connected>(&event.body);
        return connected != nullptr && connected->tls && connected->resumed;
    }, 3s));
    engine.stop();
}
