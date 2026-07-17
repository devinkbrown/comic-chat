#include "comicchat/net/connection_engine.hpp"
#include "comicchat/thread_compat.hpp"
#include "comicchat/net/flood.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
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
#include <sys/time.h>
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

enum class ServerMode { plaintext_echo, plaintext_burst, plaintext_sink, tls_echo, tls_stall };
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
        thread_ = comicchat::threading::JThread{[this](const comicchat::threading::StopToken token) { run(token); }};
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
    void run(const comicchat::threading::StopToken token) {
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
        if (mode_ == ServerMode::plaintext_sink) {
            std::array<std::byte, 256> input{};
            while (!token.stop_requested() && ::recv(accepted, input.data(), input.size(), 0) > 0) {}
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
    comicchat::threading::JThread thread_;
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
        thread_ = comicchat::threading::JThread{[this](const comicchat::threading::StopToken token) { run(token); }};
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
    [[nodiscard]] auto socks_request() const -> std::vector<std::byte> {
        std::scoped_lock lock{request_mutex_};
        return socks_request_;
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

    void run(const comicchat::threading::StopToken token) {
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
            std::array<std::byte, 4> request{};
            if (!read_exact(accepted, request)) return;
            std::vector<std::byte> target;
            bool domain{};
            if (request[3] == std::byte{1}) target.resize(4 + 2);
            else if (request[3] == std::byte{4}) target.resize(16 + 2);
            else if (request[3] == std::byte{3}) {
                std::array<std::byte, 1> length{};
                if (!read_exact(accepted, length)) return;
                target.push_back(length[0]);
                target.resize(1 + std::to_integer<unsigned char>(length[0]) + 2);
                domain = true;
            } else return;
            auto target_span = std::span<std::byte>{target};
            if (!read_exact(accepted, domain ? target_span.subspan(1) : target_span)) return;
            {
                std::scoped_lock lock{request_mutex_};
                socks_request_.assign(request.begin(), request.end());
                socks_request_.insert(socks_request_.end(), target.begin(), target.end());
            }
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
    std::vector<std::byte> socks_request_;
    comicchat::threading::JThread thread_;
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
        thread_ = comicchat::threading::JThread{[this](const comicchat::threading::StopToken token) {
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
    comicchat::threading::JThread thread_;
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
        thread_ = comicchat::threading::JThread{[this](const comicchat::threading::StopToken token) { run(token); }};
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
    void run(const comicchat::threading::StopToken token) {
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
    comicchat::threading::JThread thread_;
};

auto serve_one_tls_session(
    const int accepted,
    const comicchat::threading::StopToken& token,
    std::atomic_bool* clean_peer_shutdown = nullptr) -> bool {
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
    static constexpr unsigned char personalization[] = "comic-chat-stale-tls";
    const std::string certificate_path = std::string{MBEDTLS_DATA_DIR} + "/server6.crt";
    const std::string key_path = std::string{MBEDTLS_DATA_DIR} + "/server6.key";
    bool established{};
    const auto initialized =
        mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy, personalization,
                              sizeof(personalization) - 1) == 0 &&
        mbedtls_x509_crt_parse_file(&certificate, certificate_path.c_str()) == 0 &&
        mbedtls_pk_parse_keyfile(&key, key_path.c_str(), nullptr,
                                 mbedtls_ctr_drbg_random, &random) == 0 &&
        mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0 &&
        mbedtls_ssl_conf_own_cert(&config, &certificate, &key) == 0;
    if (initialized && !token.stop_requested()) {
        mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &random);
        if (mbedtls_ssl_setup(&ssl, &config) == 0) {
            mbedtls_ssl_set_bio(&ssl, &client, mbedtls_net_send, mbedtls_net_recv, nullptr);
            established = mbedtls_ssl_handshake(&ssl) == 0;
            if (established && clean_peer_shutdown != nullptr) {
                std::array<unsigned char, 1> input{};
                clean_peer_shutdown->store(
                    mbedtls_ssl_read(&ssl, input.data(), input.size()) ==
                    MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY);
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
    return established;
}

// Holds an established TLS socket open until the client shuts down, proving
// that the established-session guard still permits close_notify on the socket
// whose keys belong to it.
class TlsCleanShutdownServer final {
public:
    TlsCleanShutdownServer() {
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
        REQUIRE(::listen(listener_, 1) == 0);
        socklen_t size = sizeof(address);
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = comicchat::threading::JThread{[this](const comicchat::threading::StopToken token) {
            const auto accepted = ::accept(listener_, nullptr, nullptr);
            if (accepted < 0 || token.stop_requested()) return;
            client_.store(accepted);
            (void)serve_one_tls_session(accepted, token, &clean_shutdown_);
            if (const auto client = client_.exchange(-1); client >= 0) {
                (void)::shutdown(client, SHUT_RDWR);
                (void)::close(client);
            }
        }};
    }

    ~TlsCleanShutdownServer() {
        thread_.request_stop();
        if (const auto client = client_.load(); client >= 0) (void)::shutdown(client, SHUT_RDWR);
        if (listener_ >= 0) (void)::shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (const auto client = client_.exchange(-1); client >= 0) (void)::close(client);
        if (listener_ >= 0) (void)::close(listener_);
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t { return port_; }

    [[nodiscard]] auto wait_for_clean_shutdown(
        const std::chrono::milliseconds timeout = 3s) const -> bool {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (clean_shutdown_.load()) return true;
            std::this_thread::sleep_for(2ms);
        }
        return false;
    }

private:
    int listener_{-1};
    std::atomic_int client_{-1};
    std::atomic_bool clean_shutdown_{};
    std::uint16_t port_{};
    comicchat::threading::JThread thread_;
};

// Drives the reconnect shape that strands TLS state: the first connection
// negotiates SOCKS5 and terminates a complete TLS session before dropping, and
// the second rejects the SOCKS5 greeting so the engine fails before any new TLS
// handshake begins. Everything the client writes on that second socket after the
// rejection is recorded, since none of it can legitimately be TLS.
class StaleTlsReconnectProxy final {
public:
    StaleTlsReconnectProxy() {
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
        REQUIRE(::listen(listener_, 2) == 0);
        socklen_t size = sizeof(address);
        REQUIRE(::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        port_ = ntohs(address.sin_port);
        thread_ = comicchat::threading::JThread{[this](const comicchat::threading::StopToken token) { run(token); }};
    }

    ~StaleTlsReconnectProxy() {
        thread_.request_stop();
        if (const auto client = client_.load(); client >= 0) (void)::shutdown(client, SHUT_RDWR);
        if (listener_ >= 0) (void)::shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (const auto client = client_.exchange(-1); client >= 0) (void)::close(client);
        if (listener_ >= 0) (void)::close(listener_);
    }

    [[nodiscard]] auto port() const noexcept -> std::uint16_t { return port_; }
    [[nodiscard]] auto tls_session_completed() const noexcept -> bool { return tls_completed_.load(); }
    [[nodiscard]] auto greeting_rejected() const noexcept -> bool { return greeting_rejected_.load(); }

    [[nodiscard]] auto bytes_after_rejection() const -> std::vector<std::byte> {
        std::scoped_lock lock{trailing_mutex_};
        return trailing_;
    }

    [[nodiscard]] auto wait_for_rejected_socket_drain(const std::chrono::milliseconds timeout = 3s) const -> bool {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (drained_.load()) return true;
            std::this_thread::sleep_for(2ms);
        }
        return false;
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

    static auto accept_socks_greeting(const int socket) -> bool {
        std::array<std::byte, 3> greeting{};
        if (!read_exact(socket, greeting)) return false;
        return greeting[0] == std::byte{5};
    }

    void run(const comicchat::threading::StopToken token) {
        const auto tunnel = ::accept(listener_, nullptr, nullptr);
        if (tunnel < 0 || token.stop_requested()) return;
        client_.store(tunnel);
        if (accept_socks_greeting(tunnel)) {
            constexpr std::array<std::byte, 2> greeting_reply{std::byte{5}, std::byte{0}};
            (void)::send(tunnel, greeting_reply.data(), greeting_reply.size(), 0);
            // An IPv4 literal target keeps the SOCKS5 connect request fixed-width.
            std::array<std::byte, 10> request{};
            if (read_exact(tunnel, request)) {
                constexpr std::array<std::byte, 10> connected{
                    std::byte{5}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{127},
                    std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0}, std::byte{1},
                };
                (void)::send(tunnel, connected.data(), connected.size(), 0);
                tls_completed_.store(serve_one_tls_session(tunnel, token));
            }
        }
        // Dropping the tunnel without close_notify leaves the client holding a
        // fully established session and sends it into reconnect.
        const auto finished = client_.exchange(-1);
        if (finished >= 0) {
            (void)::shutdown(finished, SHUT_RDWR);
            (void)::close(finished);
        }
        if (token.stop_requested()) return;

        const auto rejected = ::accept(listener_, nullptr, nullptr);
        if (rejected < 0 || token.stop_requested()) return;
        client_.store(rejected);
        if (accept_socks_greeting(rejected)) {
            constexpr std::array<std::byte, 2> refusal{std::byte{5}, std::byte{0xff}};
            (void)::send(rejected, refusal.data(), refusal.size(), 0);
            greeting_rejected_.store(true);
            timeval timeout{};
            timeout.tv_sec = 2;
            (void)::setsockopt(rejected, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            std::array<std::byte, 512> input{};
            while (!token.stop_requested()) {
                const auto count = ::recv(rejected, input.data(), input.size(), 0);
                if (count <= 0) break;
                std::scoped_lock lock{trailing_mutex_};
                trailing_.insert(trailing_.end(), input.begin(), input.begin() + count);
            }
        }
        drained_.store(true);
    }

    int listener_{-1};
    std::atomic_int client_{-1};
    std::atomic_bool tls_completed_{};
    std::atomic_bool greeting_rejected_{};
    std::atomic_bool drained_{};
    std::uint16_t port_{};
    mutable std::mutex trailing_mutex_;
    std::vector<std::byte> trailing_;
    comicchat::threading::JThread thread_;
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

TEST_CASE("flood windows use monotonic time without counter rollover") {
    using comicchat::net::FloodThreshold;
    comicchat::net::MonotonicFloodWindow window;
    const auto start = comicchat::net::MonotonicFloodWindow::clock::time_point{1s};
    CHECK_FALSE(window.record_at(start, 3, 4s, FloodThreshold::at_limit));
    CHECK_FALSE(window.record_at(start + 1s, 3, 4s, FloodThreshold::at_limit));
    CHECK(window.record_at(start + 2s, 3, 4s, FloodThreshold::at_limit));
    CHECK_FALSE(window.record_at(start + 10s, 3, 4s, FloodThreshold::at_limit));
    // A clock anomaly resets rather than using an absolute wrapped delta.
    CHECK_FALSE(window.record_at(start, 3, 4s, FloodThreshold::at_limit));

    window.reset();
    CHECK_FALSE(window.record_at(start, 2, 4s, FloodThreshold::over_limit));
    CHECK_FALSE(window.record_at(start + 1s, 2, 4s, FloodThreshold::over_limit));
    CHECK(window.record_at(start + 2s, 2, 4s, FloodThreshold::over_limit));
}

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

TEST_CASE("completion reservations hard-bound an unpolled event queue") {
    LoopbackServer server{ServerMode::plaintext_sink};
    auto options = options_for(server, comicchat::net::Security::plaintext);
    options.limits.queued_commands = 8;
    options.limits.transmit_bytes = 1024;
    comicchat::net::ConnectionEngine engine;
    const auto generation = engine.start(options);
    REQUIRE(generation);
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));

    std::size_t accepted{};
    for (std::uint64_t id = 1; id <= 1000; ++id) {
        if (engine.post(comicchat::net::Send{*generation, id, comicchat::net::Priority::control,
                                             bytes("x"), false})) ++accepted;
    }
    CHECK(accepted <= options.limits.queued_commands - 2);
    REQUIRE(accepted > 0);

    std::this_thread::sleep_for(100ms);
    CHECK_FALSE(engine.post(comicchat::net::Send{*generation, 1001, comicchat::net::Priority::control,
                                                  bytes("x"), false}));
    const auto queued = engine.poll_events(128);
    CHECK(queued.size() <= options.limits.queued_commands);
    CHECK(static_cast<std::size_t>(std::ranges::count_if(queued, [](const auto& event) {
        return std::holds_alternative<comicchat::net::SendComplete>(event.body);
    })) == accepted);
    CHECK(engine.stats().peak_queued_events <= options.limits.queued_commands);
    engine.stop();
}

TEST_CASE("explicit disconnect closes admission and releases reserved event slots") {
    LoopbackServer server{ServerMode::plaintext_sink};
    auto options = options_for(server, comicchat::net::Security::plaintext);
    options.limits.queued_commands = 8;
    comicchat::net::ConnectionEngine engine;
    const auto generation = engine.start(options);
    REQUIRE(generation);
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));

    REQUIRE(engine.post(comicchat::net::Send{*generation, 1, comicchat::net::Priority::chat,
                                             bytes("queued-before-close"), false}));
    REQUIRE(engine.post(comicchat::net::Disconnect{*generation, "test disconnect"}));
    REQUIRE(wait_for(engine, [](const auto& event) {
        const auto* closed = std::get_if<comicchat::net::Closed>(&event.body);
        return closed != nullptr && closed->retry_after == 0ms;
    }));
    CHECK(engine.post(comicchat::net::Send{*generation, 2, comicchat::net::Priority::authentication,
                                           bytes("credential"), true}) ==
          std::unexpected{comicchat::net::EngineError::not_running});
    engine.stop();
}

TEST_CASE("throwing and reentrant wakeup callbacks do not terminate the worker") {
    LoopbackServer server{ServerMode::plaintext_sink};
    comicchat::net::ConnectionEngine engine;
    const auto generation = engine.start(options_for(server, comicchat::net::Security::plaintext));
    REQUIRE(generation);
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));

    engine.set_wakeup([] { throw std::runtime_error{"test notifier"}; });
    REQUIRE(engine.post(comicchat::net::Send{*generation, 40, comicchat::net::Priority::control,
                                             bytes("a"), false}));
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::SendComplete>(event.body);
    }));

    std::atomic_bool callback_stopped{};
    engine.set_wakeup([&] {
        if (!callback_stopped.exchange(true)) engine.stop();
    });
    REQUIRE(engine.post(comicchat::net::Send{*generation, 41, comicchat::net::Priority::control,
                                             bytes("b"), false}));
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!callback_stopped.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    REQUIRE(callback_stopped.load());
    const auto started = std::chrono::steady_clock::now();
    engine.stop();
    CHECK(std::chrono::steady_clock::now() - started < 500ms);
}

TEST_CASE("a network-thread stop cannot restart over its own joinable worker") {
    LoopbackServer server{ServerMode::plaintext_sink};
    auto options = options_for(server, comicchat::net::Security::plaintext);
    comicchat::net::ConnectionEngine engine;
    const auto generation = engine.start(options);
    REQUIRE(generation);
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));

    std::atomic_int restart_result{};
    engine.set_wakeup([&] {
        if (restart_result.load(std::memory_order_relaxed) != 0) return;
        engine.stop();
        const auto restarted = engine.start(options);
        restart_result.store(
            !restarted && restarted.error() == comicchat::net::EngineError::already_running ? 1 : -1,
            std::memory_order_release);
    });
    REQUIRE(engine.post(comicchat::net::Send{*generation, 42, comicchat::net::Priority::control,
                                             bytes("trigger"), false}));
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (restart_result.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    CHECK(restart_result.load(std::memory_order_acquire) == 1);
    engine.stop();

    LoopbackServer replacement{ServerMode::plaintext_sink};
    const auto restarted =
        engine.start(options_for(replacement, comicchat::net::Security::plaintext));
    REQUIRE(restarted);
    CHECK(*restarted == *generation + 1);
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));
    engine.stop();
}

TEST_CASE("start rejects an already running connection without changing generations") {
    LoopbackServer server{ServerMode::plaintext_sink};
    auto options = options_for(server, comicchat::net::Security::plaintext);
    comicchat::net::ConnectionEngine engine;
    const auto generation = engine.start(options);
    REQUIRE(generation);
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));
    CHECK(engine.start(options) ==
          std::unexpected{comicchat::net::EngineError::already_running});
    CHECK(engine.generation() == *generation);
    engine.stop();
}

TEST_CASE("moved-from connection wrappers remain reusable stopped objects") {
    LoopbackServer active_server{ServerMode::plaintext_sink};
    comicchat::net::ConnectionEngine source;
    const auto active_generation =
        source.start(options_for(active_server, comicchat::net::Security::plaintext));
    REQUIRE(active_generation);
    REQUIRE(wait_for(source, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));

    comicchat::net::ConnectionEngine active{std::move(source)};
    CHECK(active.generation() == *active_generation);
    source.stop();
    CHECK(source.generation() == 0);
    CHECK(source.stats().queued_commands == 0);
    CHECK(source.poll_events().empty());
    CHECK(source.post(comicchat::net::Send{0, 1, comicchat::net::Priority::authentication,
                                           bytes("discarded"), true}) ==
          std::unexpected{comicchat::net::EngineError::not_running});

    LoopbackServer replacement_server{ServerMode::plaintext_sink};
    const auto replacement_generation =
        source.start(options_for(replacement_server, comicchat::net::Security::plaintext));
    REQUIRE(replacement_generation);
    REQUIRE(wait_for(source, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));
    source.stop();
    active.stop();
}

TEST_CASE("poll wakeups stay synchronized with async-handle teardown") {
    comicchat::net::ConnectionOptions options;
    options.endpoint = {"127.0.0.1", 1};
    options.security = comicchat::net::Security::plaintext;
    options.deadlines.connect = 100ms;
    options.deadlines.handshake = 100ms;
    options.deadlines.idle = 1s;
    options.deadlines.ping = 500ms;

    for (unsigned int iteration = 0; iteration < 64; ++iteration) {
        comicchat::net::ConnectionEngine engine;
        REQUIRE(engine.start(options));
        std::atomic_bool poll{};
        comicchat::threading::JThread poller{[&](const comicchat::threading::StopToken token) {
            while (!token.stop_requested()) {
                (void)engine.poll_events(1);
                poll.store(true, std::memory_order_release);
            }
        }};
        const auto deadline = std::chrono::steady_clock::now() + 250ms;
        while (!poll.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(poll.load(std::memory_order_acquire));
        engine.stop();
        poller.request_stop();
        poller.join();
    }
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

TEST_CASE("SOCKS5 emits numeric IPv4 and IPv6 address types without DNS ambiguity") {
    const auto check_target = [](const std::string_view host, const std::byte address_type,
                                 const std::span<const std::byte> address) {
        ProxyServer proxy{ProxyMode::socks5};
        comicchat::net::ConnectionOptions options;
        options.endpoint = {std::string{host}, 6667};
        options.security = comicchat::net::Security::plaintext;
        options.proxy.kind = comicchat::net::ProxyKind::socks5;
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
        const auto request = proxy.socks_request();
        REQUIRE(request.size() == 4 + address.size() + 2);
        CHECK(request[0] == std::byte{5});
        CHECK(request[1] == std::byte{1});
        CHECK(request[2] == std::byte{0});
        CHECK(request[3] == address_type);
        CHECK(std::equal(address.begin(), address.end(), request.begin() + 4));
        CHECK(request[request.size() - 2] == std::byte{0x1a});
        CHECK(request.back() == std::byte{0x0b});
        engine.stop();
    };

    constexpr std::array ipv4{std::byte{127}, std::byte{0}, std::byte{0}, std::byte{1}};
    std::array<std::byte, 16> ipv6{};
    ipv6.back() = std::byte{1};
    check_target("127.0.0.1", std::byte{1}, ipv4);
    check_target("::1", std::byte{4}, ipv6);
}

TEST_CASE("proxy authentication validates RFC1929 and HTTP Basic user identifiers") {
    comicchat::net::ConnectionOptions options;
    options.endpoint = {"irc.example", 6667};
    options.security = comicchat::net::Security::plaintext;
    options.proxy.host = "127.0.0.1";
    options.proxy.port = 1080;
    options.proxy.username = "";
    options.proxy.password = "secret";

    comicchat::net::ConnectionEngine engine;
    options.proxy.kind = comicchat::net::ProxyKind::socks5;
    CHECK(engine.start(options) == std::unexpected{comicchat::net::EngineError::invalid_options});
    options.proxy.username = "user";
    options.proxy.password = "";
    CHECK(engine.start(options) == std::unexpected{comicchat::net::EngineError::invalid_options});
    options.proxy.kind = comicchat::net::ProxyKind::http_connect;
    options.proxy.username = "user:name";
    options.proxy.password = "secret";
    CHECK(engine.start(options) == std::unexpected{comicchat::net::EngineError::invalid_options});
}

TEST_CASE("idle connections emit one application-owned ping deadline event") {
    LoopbackServer server{ServerMode::plaintext_sink};
    auto options = options_for(server, comicchat::net::Security::plaintext);
    options.deadlines.ping = 40ms;
    options.deadlines.idle = 250ms;
    comicchat::net::ConnectionEngine engine;
    REQUIRE(engine.start(options));
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::PingDue>(event.body);
    }, 500ms));
    std::this_thread::sleep_for(80ms);
    const auto extra = engine.poll_events();
    CHECK(std::none_of(extra.begin(), extra.end(), [](const auto& event) {
        return std::holds_alternative<comicchat::net::PingDue>(event.body);
    }));
    engine.stop();
}

TEST_CASE("outbound traffic cannot suppress server liveness deadlines") {
    LoopbackServer server{ServerMode::plaintext_sink};
    auto options = options_for(server, comicchat::net::Security::plaintext);
    options.deadlines.ping = 40ms;
    options.deadlines.idle = 160ms;
    options.reconnect_jitter_seed = 1;
    comicchat::net::ConnectionEngine engine;
    const auto generation = engine.start(options);
    REQUIRE(generation);
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));

    bool ping_due{};
    bool idle_timeout{};
    std::uint64_t send_id = 100;
    const auto deadline = std::chrono::steady_clock::now() + 400ms;
    while (std::chrono::steady_clock::now() < deadline && !idle_timeout) {
        (void)engine.post(comicchat::net::Send{*generation, send_id++,
            comicchat::net::Priority::control, bytes("outbound"), false});
        for (const auto& event : engine.poll_events(128)) {
            ping_due = ping_due || std::holds_alternative<comicchat::net::PingDue>(event.body);
            if (const auto* diagnostic = std::get_if<comicchat::net::Diagnostic>(&event.body))
                idle_timeout = idle_timeout || diagnostic->code == "idle-timeout";
        }
        std::this_thread::sleep_for(10ms);
    }
    CHECK(ping_due);
    CHECK(idle_timeout);
    engine.stop();
}

TEST_CASE("per-target chat buckets defer floods without starving another target") {
    LoopbackServer server{ServerMode::plaintext_sink};
    auto options = options_for(server, comicchat::net::Security::plaintext);
    options.deadlines.ping = 2s;
    comicchat::net::ConnectionEngine engine;
    const auto generation = engine.start(options);
    REQUIRE(generation);
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::Connected>(event.body);
    }));

    for (std::uint64_t id = 1; id <= 8; ++id) {
        comicchat::net::Send send{*generation, id, comicchat::net::Priority::chat,
            bytes("same-target"), false};
        send.target = "#busy";
        REQUIRE(engine.post(std::move(send)));
    }
    comicchat::net::Send fair{*generation, 100, comicchat::net::Priority::chat,
        bytes("other-target"), false};
    fair.target = "#other";
    REQUIRE(engine.post(std::move(fair)));
    CHECK(wait_for(engine, [](const auto& event) {
        const auto* complete = std::get_if<comicchat::net::SendComplete>(&event.body);
        return complete != nullptr && complete->id == 100;
    }, 700ms));
    CHECK(engine.stats().target_throttle_deferrals > 0);
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

TEST_CASE("a proxy reconnect that fails before TLS emits no prior-session close_notify") {
    StaleTlsReconnectProxy proxy;
    comicchat::net::ConnectionOptions options;
    options.endpoint = {"127.0.0.1", 6667};
    options.security = comicchat::net::Security::tls;
    options.server_name = "localhost";
    options.ca_file = std::string{MBEDTLS_DATA_DIR} + "/test-ca2.crt";
    options.proxy.kind = comicchat::net::ProxyKind::socks5;
    options.proxy.host = "127.0.0.1";
    options.proxy.port = proxy.port();
    options.deadlines.connect = 1s;
    options.deadlines.handshake = 1s;
    options.deadlines.idle = 5s;
    comicchat::net::ConnectionEngine engine;
    REQUIRE(engine.start(options).has_value());
    REQUIRE(wait_for(engine, [](const auto& event) {
        const auto* connected = std::get_if<comicchat::net::Connected>(&event.body);
        return connected != nullptr && connected->tls;
    }));
    REQUIRE(wait_for(engine, [](const auto& event) {
        const auto* diagnostic = std::get_if<comicchat::net::Diagnostic>(&event.body);
        return diagnostic != nullptr && diagnostic->code == "proxy-auth";
    }, 5s));
    REQUIRE(proxy.wait_for_rejected_socket_drain());
    CHECK(proxy.tls_session_completed());
    CHECK(proxy.greeting_rejected());
    // TLS state stranded past its socket encrypts close_notify under the dropped
    // session and hands the alert to the proxy that just refused the tunnel.
    CHECK(proxy.bytes_after_rejection().empty());
    engine.stop();
}

TEST_CASE("intentional TLS shutdown sends close_notify only on its established socket") {
    TlsCleanShutdownServer server;
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
        return connected != nullptr && connected->tls;
    }));
    engine.stop();
    CHECK(server.wait_for_clean_shutdown());
}
