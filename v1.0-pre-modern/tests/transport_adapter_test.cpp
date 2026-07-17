#include "../transportadapter.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using comic_chat::v1::transport::AdapterError;
using comic_chat::v1::transport::EventAction;
using comic_chat::v1::transport::SessionGate;
using comic_chat::v1::transport::SessionPhase;
using comic_chat::v1::transport::WakeupGate;

comicchat::net::Event Connected(comicchat::net::GenerationId generation)
{
	return {generation, comicchat::net::Connected{"irc.example", "127.0.0.1", true, false}};
}

comicchat::net::Event Bytes(comicchat::net::GenerationId generation)
{
	auto bytes = std::make_shared<const std::vector<std::byte>>(
		std::vector<std::byte>{std::byte{'O'}, std::byte{'K'}});
	return {generation, comicchat::net::BytesReceived{std::move(bytes)}};
}

comicchat::net::Event Closed(comicchat::net::GenerationId generation,
	std::chrono::milliseconds retry_after)
{
	return {generation, comicchat::net::Closed{"test", retry_after}};
}

void TestGenerationAndOrdering()
{
	SessionGate gate;
	gate.Begin(41);
	assert(gate.phase() == SessionPhase::starting);
	assert(gate.Classify(Connected(40)) == EventAction::stale);
	assert(gate.Classify(Bytes(41)) == EventAction::out_of_order);
	assert(gate.Classify(Connected(41)) == EventAction::connected);
	assert(gate.phase() == SessionPhase::connected);
	assert(gate.Classify(Connected(41)) == EventAction::out_of_order);
	assert(gate.Classify(Bytes(41)) == EventAction::bytes_received);

	assert(gate.Classify(Closed(41, std::chrono::milliseconds{250})) ==
		EventAction::disconnected);
	assert(gate.phase() == SessionPhase::reconnecting);
	assert(gate.Classify(Bytes(41)) == EventAction::out_of_order);
	assert(gate.Classify(Connected(41)) == EventAction::connected);
	assert(gate.Classify(Closed(41, std::chrono::milliseconds::zero())) ==
		EventAction::disconnected);
	assert(gate.phase() == SessionPhase::stopped);
	assert(gate.Classify(Connected(41)) == EventAction::out_of_order);

	gate.Begin(42);
	assert(gate.Classify(Closed(41, std::chrono::milliseconds::zero())) ==
		EventAction::stale);
	assert(gate.Classify(Closed(42, std::chrono::milliseconds{100})) ==
		EventAction::connect_failed);
	gate.Stop();
	assert(!gate.active());
}

void TestWakeupCoalescingAndCookieIsolation()
{
	WakeupGate gate;
	gate.Reset(77);
	assert(!gate.TryMarkPending(76));
	assert(gate.TryMarkPending(77));
	assert(gate.pending());
	assert(!gate.TryMarkPending(77));
	assert(!gate.BeginDrain(76));
	assert(gate.pending());
	assert(gate.BeginDrain(77));
	assert(!gate.pending());
	assert(gate.TryMarkPending(77));
	gate.CancelPending(76);
	assert(gate.pending());
	gate.CancelPending(77);
	assert(!gate.pending());
	gate.Disable();
	assert(!gate.TryMarkPending(77));
	assert(!gate.BeginDrain(77));

	gate.Reset(88);
	std::atomic<bool> start{false};
	std::atomic<unsigned int> winners{};
	{
		std::vector<std::jthread> workers;
		workers.reserve(16);
		for (unsigned int index = 0; index < 16; ++index) {
			workers.emplace_back([&] {
				start.wait(false, std::memory_order_acquire);
				if (gate.TryMarkPending(88))
					winners.fetch_add(1, std::memory_order_relaxed);
			});
		}
		start.store(true, std::memory_order_release);
		start.notify_all();
	}
	assert(winners.load(std::memory_order_relaxed) == 1);
}

void TestBoundedOutboundFacade()
{
	comic_chat::ircv3::Engine engine;
	const auto registration = engine.BeginRegistration({}, "Tiki", true);
	assert(!registration.empty());

	auto chat = comic_chat::v1::transport::PrepareOutbound(
		engine, "PRIVMSG #ink :hello\r\n", 9, 1);
	assert(chat.has_value());
	assert(chat->generation == 9);
	assert(chat->id == 1);
	assert(chat->priority == comicchat::net::Priority::chat);
	assert(!chat->sensitive);
	assert(chat->target == "#ink");
	assert(!chat->bytes.empty());

	auto authentication = comic_chat::v1::transport::PrepareOutbound(
		engine, "AUTHENTICATE secret\r\n", 9, 2);
	assert(authentication.has_value());
	assert(authentication->priority == comicchat::net::Priority::authentication);
	assert(authentication->sensitive);

	auto keepalive = comic_chat::v1::transport::PrepareOutbound(
		engine, "PONG :token\r\n", 9, 3);
	assert(keepalive.has_value());
	assert(keepalive->priority == comicchat::net::Priority::pong);

	const std::string oversized(
		comic_chat::v1::transport::maximum_irc_wire_bytes + 1, 'x');
	auto rejected_size = comic_chat::v1::transport::PrepareOutbound(
		engine, oversized, 9, 4);
	assert(!rejected_size.has_value());
	assert(rejected_size.error() == AdapterError::line_too_long);

	auto rejected_generation = comic_chat::v1::transport::PrepareOutbound(
		engine, "PING :token\r\n", 0, 5);
	assert(!rejected_generation.has_value());
	assert(rejected_generation.error() == AdapterError::not_open);

	constexpr char contains_nul_wire[] = "PRIVMSG #ink :bad\0line\r\n";
	const std::string contains_nul{contains_nul_wire, sizeof(contains_nul_wire) - 1};
	auto rejected_line = comic_chat::v1::transport::PrepareOutbound(
		engine, contains_nul, 9, 6);
	assert(!rejected_line.has_value());
	assert(rejected_line.error() == AdapterError::invalid_line);
}

void TestBoundedLegacyInboundFacade()
{
	comic_chat::ircv3::Message message;
	message.prefix = "nick!user@host.example";
	message.command = "PRIVMSG";
	message.params = {"#ink", "hello"};
	auto prepared = comic_chat::v1::transport::PrepareLegacyInbound(message);
	assert(prepared.has_value());
	assert(prepared->starts_with(":nick!user@host.example PRIVMSG #ink :hello"));
	message.params[1] = "hello world";
	prepared = comic_chat::v1::transport::PrepareLegacyInbound(message);
	assert(prepared && prepared->ends_with("PRIVMSG #ink :hello world\r\n"));

	message.command = "NICK";
	message.params = {"newnick"};
	prepared = comic_chat::v1::transport::PrepareLegacyInbound(message);
	assert(prepared && prepared->ends_with("NICK :newnick\r\n"));
	message.command = "353";
	message.params = {"me", "=", "#ink", "one two"};
	prepared = comic_chat::v1::transport::PrepareLegacyInbound(message);
	assert(prepared && prepared->ends_with("353 me = #ink :one two\r\n"));
	message.command = "KICK";
	message.params = {"#ink", "nick"};
	prepared = comic_chat::v1::transport::PrepareLegacyInbound(message);
	assert(prepared && prepared->ends_with("KICK #ink nick\r\n"));
	message.params.push_back("reason");
	prepared = comic_chat::v1::transport::PrepareLegacyInbound(message);
	assert(prepared && prepared->ends_with("KICK #ink nick :reason\r\n"));
	message.command = "ERROR";
	message.params = {"closing"};
	prepared = comic_chat::v1::transport::PrepareLegacyInbound(message);
	assert(prepared && prepared->ends_with("ERROR :closing\r\n"));

	message.prefix = std::string(
		comic_chat::v1::transport::maximum_legacy_prefix_component_bytes, 'n') +
		"!" + std::string(
			comic_chat::v1::transport::maximum_legacy_prefix_component_bytes, 'u') +
		"@" + std::string(
			comic_chat::v1::transport::maximum_legacy_prefix_component_bytes, 'h');
	assert(comic_chat::v1::transport::PrepareLegacyInbound(message).has_value());

	message.prefix = std::string(
		comic_chat::v1::transport::maximum_legacy_prefix_component_bytes + 1, 'n');
	auto long_nickname = comic_chat::v1::transport::PrepareLegacyInbound(message);
	assert(!long_nickname.has_value());
	assert(long_nickname.error() == AdapterError::invalid_line);

	message.prefix = "nick!" + std::string(
		comic_chat::v1::transport::maximum_legacy_prefix_component_bytes + 1, 'u') + "@host";
	assert(!comic_chat::v1::transport::PrepareLegacyInbound(message).has_value());
	message.prefix = "nick!user@" + std::string(
		comic_chat::v1::transport::maximum_legacy_prefix_component_bytes + 1, 'h');
	assert(!comic_chat::v1::transport::PrepareLegacyInbound(message).has_value());

	message.prefix = "nick!user@host";
	message.command = "PRIVMSG";
	message.params = {"#ink", std::string(comic_chat::v1::transport::maximum_legacy_wire_bytes, 'x')};
	auto oversized = comic_chat::v1::transport::PrepareLegacyInbound(message);
	assert(!oversized.has_value());
	assert(oversized.error() == AdapterError::invalid_line);
}

} // namespace

int main()
{
	TestGenerationAndOrdering();
	TestWakeupCoalescingAndCookieIsolation();
	TestBoundedOutboundFacade();
	TestBoundedLegacyInboundFacade();
	return 0;
}
