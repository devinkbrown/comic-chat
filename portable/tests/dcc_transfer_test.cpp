#include "comicchat/net/dcc_transfer_engine.hpp"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace std::chrono_literals;

namespace {

auto bytes(const std::string_view value) -> std::vector<std::byte> {
    std::vector<std::byte> output(value.size());
    std::memcpy(output.data(), value.data(), value.size());
    return output;
}

auto wait_for(comicchat::net::DccTransferEngine& engine,
              const std::function<bool(const comicchat::net::DccEvent&)>& predicate,
              const std::chrono::milliseconds timeout = 3s)
    -> std::optional<comicchat::net::DccEvent> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& event : engine.poll_events(1)) if (predicate(event)) return std::move(event);
        std::this_thread::sleep_for(2ms);
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("legacy DCC IPv4 decimal conversion is strict and host ordered") {
    CHECK(comicchat::net::dcc_legacy_ipv4_decimal("127.0.0.1") == 2'130'706'433U);
    CHECK(comicchat::net::dcc_legacy_ipv4_decimal("192.0.2.4") == 3'221'225'988U);
    CHECK_FALSE(comicchat::net::dcc_legacy_ipv4_decimal("irc.example").has_value());
    CHECK_FALSE(comicchat::net::dcc_legacy_ipv4_decimal("224.0.0.1").has_value());
    CHECK_FALSE(comicchat::net::dcc_legacy_ipv4_decimal("0.0.0.0").has_value());
}

TEST_CASE("DCC IPv4 scope separates local-service targets from public offers") {
    using comicchat::net::DccAddressScope;
    CHECK(comicchat::net::dcc_ipv4_scope("127.42.0.1") == DccAddressScope::loopback);
    CHECK(comicchat::net::dcc_ipv4_scope("169.254.10.20") == DccAddressScope::link_local);
    CHECK(comicchat::net::dcc_ipv4_scope("10.0.0.1") == DccAddressScope::private_network);
    CHECK(comicchat::net::dcc_ipv4_scope("172.31.255.254") == DccAddressScope::private_network);
    CHECK(comicchat::net::dcc_ipv4_scope("192.168.1.2") == DccAddressScope::private_network);
    CHECK(comicchat::net::dcc_ipv4_scope("100.64.0.1") == DccAddressScope::private_network);
    CHECK(comicchat::net::dcc_ipv4_scope("0.9.8.7") == DccAddressScope::unspecified);
    CHECK(comicchat::net::dcc_ipv4_scope("224.0.0.1") == DccAddressScope::multicast);
    CHECK(comicchat::net::dcc_ipv4_scope("255.255.255.255") == DccAddressScope::limited_broadcast);
    CHECK(comicchat::net::dcc_ipv4_scope("240.0.0.1") == DccAddressScope::reserved);
    CHECK(comicchat::net::dcc_ipv4_scope("192.0.2.1") == DccAddressScope::reserved);
    CHECK(comicchat::net::dcc_ipv4_scope("198.51.100.9") == DccAddressScope::reserved);
    CHECK(comicchat::net::dcc_ipv4_scope("203.0.113.7") == DccAddressScope::reserved);
    CHECK(comicchat::net::dcc_ipv4_scope("198.18.0.1") == DccAddressScope::reserved);
    CHECK(comicchat::net::dcc_ipv4_scope("8.8.8.8") == DccAddressScope::public_unicast);
    CHECK_FALSE(comicchat::net::dcc_ipv4_scope("not-an-address").has_value());
}

TEST_CASE("real libuv DCC SEND and RECEIVE stream with write-gated cumulative ACKs") {
    constexpr std::string_view payload{"hello world"};
    comicchat::net::DccTransferEngine sender;
    comicchat::net::DccListenOptions listen;
    listen.bind_address = "127.0.0.1";
    listen.expected_peer_address = "127.0.0.1";
    listen.file_size = payload.size();
    listen.deadlines.accept = 1s;
    listen.deadlines.idle = 1s;
    const auto send_handle = sender.start_listen(listen);
    REQUIRE(send_handle.has_value());

    const auto listening_event = wait_for(sender, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccListening>(event.body);
    });
    REQUIRE(listening_event.has_value());
    const auto listening = std::get<comicchat::net::DccListening>(listening_event->body);
    CHECK(listening.advertise_address == "127.0.0.1");
    CHECK(listening.legacy_ipv4_decimal == 2'130'706'433U);
    REQUIRE(listening.port != 0);

    comicchat::net::DccTransferEngine receiver;
    comicchat::net::DccConnectOptions connect;
    connect.peer_address = "127.0.0.1";
    connect.port = listening.port;
    connect.file_size = payload.size();
    connect.deadlines.connect = 1s;
    connect.deadlines.idle = 1s;
    const auto receive_handle = receiver.start_connect(connect);
    REQUIRE(receive_handle.has_value());
    const auto offered_event = wait_for(sender, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccPeerOffered>(event.body);
    });
    REQUIRE(offered_event.has_value());
    const auto offered = std::get<comicchat::net::DccPeerOffered>(offered_event->body);
    CHECK(offered.peer_address == "127.0.0.1");
    REQUIRE(sender.post(comicchat::net::DccAcceptPeer{*send_handle, offered.peer}));
    REQUIRE(wait_for(sender, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccPeerConnected>(event.body);
    }));
    REQUIRE(wait_for(receiver, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccPeerConnected>(event.body);
    }));

    REQUIRE(sender.post(comicchat::net::DccQueueChunk{*send_handle, bytes("hello "), false}));
    REQUIRE(sender.post(comicchat::net::DccQueueChunk{*send_handle, bytes("world"), true}));

    std::string received;
    while (received.size() < payload.size()) {
        const auto chunk_event = wait_for(receiver, [](const auto& event) {
            return std::holds_alternative<comicchat::net::DccChunkReceived>(event.body);
        });
        REQUIRE(chunk_event.has_value());
        const auto& chunk = std::get<comicchat::net::DccChunkReceived>(chunk_event->body);
        REQUIRE(chunk.bytes);
        received.append(reinterpret_cast<const char*>(chunk.bytes->data()), chunk.bytes->size());
        REQUIRE(receiver.post(comicchat::net::DccCommitReceived{
            *receive_handle, chunk.offset + chunk.bytes->size()}));
    }
    CHECK(received == payload);

    bool acknowledged{};
    bool sender_completed{};
    const auto sender_deadline = std::chrono::steady_clock::now() + 3s;
    while ((!acknowledged || !sender_completed) && std::chrono::steady_clock::now() < sender_deadline) {
        for (const auto& event : sender.poll_events()) {
            if (const auto* progress = std::get_if<comicchat::net::DccProgress>(&event.body)) {
                acknowledged = acknowledged ||
                    (progress->transferred == payload.size() && progress->peer_committed == payload.size());
            }
            sender_completed = sender_completed || std::holds_alternative<comicchat::net::DccCompleted>(event.body);
        }
        std::this_thread::sleep_for(2ms);
    }
    CHECK(acknowledged);
    CHECK(sender_completed);
    CHECK(wait_for(receiver, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccCompleted>(event.body);
    }));
    sender.stop();
    receiver.stop();
}

TEST_CASE("DCC validates addresses, bounds queued data, and cancels promptly") {
    comicchat::net::DccTransferEngine invalid_listener;
    comicchat::net::DccListenOptions wildcard;
    wildcard.file_size = 1;
    CHECK(invalid_listener.start_listen(wildcard) ==
          std::unexpected{comicchat::net::DccError::invalid_address});

    comicchat::net::DccTransferEngine invalid_connector;
    comicchat::net::DccConnectOptions multicast;
    multicast.peer_address = "224.0.0.1";
    multicast.port = 7000;
    multicast.file_size = 1;
    CHECK(invalid_connector.start_connect(multicast) ==
          std::unexpected{comicchat::net::DccError::invalid_address});

    comicchat::net::DccTransferEngine sender;
    comicchat::net::DccListenOptions listen;
    listen.bind_address = "127.0.0.1";
    listen.file_size = 5;
    listen.limits.maximum_queued_send_bytes = 4;
    const auto handle = sender.start_listen(listen);
    REQUIRE(handle.has_value());
    CHECK(sender.post(comicchat::net::DccQueueChunk{*handle, bytes("12345"), true}) ==
          std::unexpected{comicchat::net::DccError::queue_full});
    REQUIRE(sender.post(comicchat::net::DccCancel{*handle, "user cancelled"}));
    CHECK(wait_for(sender, [](const auto& event) {
        const auto* closed = std::get_if<comicchat::net::DccClosed>(&event.body);
        return closed != nullptr && closed->reason == "user cancelled";
    }));
    sender.stop();
}

TEST_CASE("DCC accept deadline is event driven") {
    comicchat::net::DccTransferEngine sender;
    comicchat::net::DccListenOptions listen;
    listen.bind_address = "127.0.0.1";
    listen.file_size = 1;
    listen.deadlines.accept = 50ms;
    const auto handle = sender.start_listen(listen);
    REQUIRE(handle.has_value());
    CHECK(wait_for(sender, [](const auto& event) {
        const auto* diagnostic = std::get_if<comicchat::net::DccDiagnostic>(&event.body);
        return diagnostic != nullptr && diagnostic->code == "accept-timeout";
    }, 1s));
    sender.stop();
}

TEST_CASE("terminal DCC transfers reject posts and restart without an explicit stop") {
    comicchat::net::DccTransferEngine engine;
    comicchat::net::DccListenOptions listen;
    listen.bind_address = "127.0.0.1";
    listen.file_size = 1;
    listen.deadlines.accept = 40ms;
    const auto first = engine.start_listen(listen);
    REQUIRE(first);
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccClosed>(event.body);
    }, 1s));
    CHECK(engine.post(comicchat::net::DccCancel{*first, "late"}) ==
        std::unexpected{comicchat::net::DccError::not_running});

    const auto second = engine.start_listen(listen);
    REQUIRE(second);
    CHECK(second->generation > first->generation);
    REQUIRE(wait_for(engine, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccListening>(event.body);
    }));
    engine.stop();
}

TEST_CASE("DCC rejects oversized arithmetic and resets every reusable generation") {
    comicchat::net::DccTransferEngine sender;
    comicchat::net::DccListenOptions listen;
    listen.bind_address = "127.0.0.1";
    listen.file_size = 5;
    const auto first = sender.start_listen(listen);
    REQUIRE(first.has_value());
    REQUIRE(wait_for(sender, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccListening>(event.body);
    }));
    CHECK(sender.post(comicchat::net::DccQueueChunk{*first, bytes("123456"), true}) ==
          std::unexpected{comicchat::net::DccError::protocol_error});
    sender.stop();

    const auto second = sender.start_listen(listen);
    REQUIRE(second.has_value());
    CHECK(second->generation > first->generation);
    CHECK(second->transfer > first->transfer);
    REQUIRE(wait_for(sender, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccListening>(event.body);
    }));
    sender.stop();

    comicchat::net::DccTransferEngine bounded;
    listen.file_size = 6;
    listen.limits.maximum_file_bytes = 5;
    CHECK(bounded.start_listen(listen) == std::unexpected{comicchat::net::DccError::invalid_options});

    listen.file_size = 1;
    listen.limits.maximum_file_bytes = 1;
    listen.limits.maximum_queued_send_bytes =
        static_cast<std::size_t>((std::numeric_limits<unsigned int>::max)());
    ++listen.limits.maximum_queued_send_bytes;
    CHECK(bounded.start_listen(listen) == std::unexpected{comicchat::net::DccError::invalid_options});
}

TEST_CASE("DCC reserves bounded diagnostic and exactly-one terminal event capacity") {
    std::atomic<unsigned int> wakeups{};
    comicchat::net::DccTransferEngine sender;
    sender.set_wakeup([&] { wakeups.fetch_add(1, std::memory_order_release); });
    comicchat::net::DccListenOptions listen;
    listen.bind_address = "127.0.0.1";
    listen.file_size = 1;
    listen.limits.maximum_events = 4;
    listen.deadlines.accept = 2s;
    const auto handle = sender.start_listen(listen);
    REQUIRE(handle.has_value());
    const auto listening_deadline = std::chrono::steady_clock::now() + 1s;
    while (wakeups.load(std::memory_order_acquire) < 1 &&
           std::chrono::steady_clock::now() < listening_deadline)
        std::this_thread::sleep_for(2ms);
    REQUIRE(wakeups.load(std::memory_order_acquire) >= 1);

    // The first offered-peer token is deterministic. Deliberately do not poll
    // Listening or PeerOffered so the two ordinary slots remain occupied.
    comicchat::net::DccTransferEngine receiver;
    comicchat::net::DccConnectOptions connect;
    connect.peer_address = "127.0.0.1";
    connect.port = 0;
    for (const auto& event : sender.poll_events(1)) {
        const auto* listening = std::get_if<comicchat::net::DccListening>(&event.body);
        REQUIRE(listening != nullptr);
        connect.port = listening->port;
    }
    REQUIRE(connect.port != 0);
    // Refill the ordinary slot consumed only to learn the ephemeral port with
    // a second non-terminal event: the peer offer.
    connect.file_size = 1;
    connect.deadlines.connect = 1s;
    connect.deadlines.idle = 2s;
    REQUIRE(receiver.start_connect(connect));
    const auto offer_deadline = std::chrono::steady_clock::now() + 1s;
    while (wakeups.load(std::memory_order_acquire) < 2 &&
           std::chrono::steady_clock::now() < offer_deadline)
        std::this_thread::sleep_for(2ms);
    REQUIRE(wakeups.load(std::memory_order_acquire) >= 2);
    REQUIRE(sender.post(comicchat::net::DccAcceptPeer{*handle, 1}));

    const auto terminal_deadline = std::chrono::steady_clock::now() + 1s;
    while (wakeups.load(std::memory_order_acquire) < 5 &&
           std::chrono::steady_clock::now() < terminal_deadline)
        std::this_thread::sleep_for(2ms);
    const auto events = sender.poll_events(100);
    CHECK(events.size() <= listen.limits.maximum_events);
    CHECK(std::count_if(events.begin(), events.end(), [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccClosed>(event.body) ||
            std::holds_alternative<comicchat::net::DccCompleted>(event.body);
    }) == 1);
    CHECK(std::count_if(events.begin(), events.end(), [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccDiagnostic>(event.body);
    }) <= 1);
    receiver.stop();
    sender.stop();
}

TEST_CASE("DCC wakeups may throw and request a self-thread stop") {
    comicchat::net::DccTransferEngine sender;
    std::atomic<unsigned int> calls{};
    sender.set_wakeup([&] {
        calls.fetch_add(1, std::memory_order_relaxed);
        sender.stop();
        throw std::runtime_error{"UI wakeup failed"};
    });
    comicchat::net::DccListenOptions listen;
    listen.bind_address = "127.0.0.1";
    listen.file_size = 1;
    REQUIRE(sender.start_listen(listen));
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (calls.load(std::memory_order_relaxed) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    REQUIRE(calls.load(std::memory_order_relaxed) != 0);
    const auto started = std::chrono::steady_clock::now();
    sender.stop();
    CHECK(std::chrono::steady_clock::now() - started < 1s);
}

TEST_CASE("rejecting an offered DCC peer retains the listener for the intended peer") {
    comicchat::net::DccTransferEngine sender;
    comicchat::net::DccListenOptions listen;
    listen.bind_address = "127.0.0.1";
    listen.file_size = 1;
    listen.deadlines.accept = 2s;
    const auto send_handle = sender.start_listen(listen);
    REQUIRE(send_handle.has_value());
    const auto listening_event = wait_for(sender, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccListening>(event.body);
    });
    REQUIRE(listening_event.has_value());
    const auto port = std::get<comicchat::net::DccListening>(listening_event->body).port;

    comicchat::net::DccConnectOptions connect;
    connect.peer_address = "127.0.0.1";
    connect.port = port;
    connect.file_size = 1;
    connect.deadlines.connect = 1s;
    connect.deadlines.idle = 2s;
    comicchat::net::DccTransferEngine rejected;
    REQUIRE(rejected.start_connect(connect));
    const auto first_offer_event = wait_for(sender, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccPeerOffered>(event.body);
    });
    REQUIRE(first_offer_event.has_value());
    const auto first_offer = std::get<comicchat::net::DccPeerOffered>(first_offer_event->body);
    REQUIRE(sender.post(comicchat::net::DccRejectPeer{*send_handle, first_offer.peer}));

    comicchat::net::DccTransferEngine accepted;
    const auto receive_handle = accepted.start_connect(connect);
    REQUIRE(receive_handle.has_value());
    const auto second_offer_event = wait_for(sender, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccPeerOffered>(event.body);
    });
    REQUIRE(second_offer_event.has_value());
    const auto second_offer = std::get<comicchat::net::DccPeerOffered>(second_offer_event->body);
    CHECK(second_offer.peer != first_offer.peer);
    REQUIRE(sender.post(comicchat::net::DccAcceptPeer{*send_handle, second_offer.peer}));
    REQUIRE(wait_for(sender, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccPeerConnected>(event.body);
    }));
    REQUIRE(sender.post(comicchat::net::DccQueueChunk{*send_handle, bytes("x"), true}));
    const auto chunk_event = wait_for(accepted, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccChunkReceived>(event.body);
    });
    REQUIRE(chunk_event.has_value());
    const auto& chunk = std::get<comicchat::net::DccChunkReceived>(chunk_event->body);
    REQUIRE(accepted.post(comicchat::net::DccCommitReceived{
        *receive_handle, chunk.offset + chunk.bytes->size()}));
    CHECK(wait_for(sender, [](const auto& event) {
        return std::holds_alternative<comicchat::net::DccCompleted>(event.body);
    }));
    rejected.stop();
    accepted.stop();
    sender.stop();
}
