#include "../transportadapter.h"

#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

void TestCheckedLegacyOutboundBuilder()
{
	using comic_chat::v1::transport::BuildLegacyOutbound;
	using comic_chat::v1::transport::maximum_legacy_wire_bytes;

	const std::array<std::string_view, 1> target{"#ink"};
	constexpr std::size_t privmsg_overhead = std::string_view{"PRIVMSG #ink :\r\n"}.size();
	std::string exact_payload(maximum_legacy_wire_bytes - privmsg_overhead, 'x');
	auto exact = BuildLegacyOutbound("PRIVMSG", target, exact_payload);
	assert(exact.has_value());
	assert(exact->size() == maximum_legacy_wire_bytes);
	assert(exact->starts_with("PRIVMSG #ink :"));
	assert(exact->ends_with("\r\n"));
	comic_chat::ircv3::Engine engine;
	assert(!engine.BeginRegistration({}, "Tiki", true).empty());
	auto exact_send = comic_chat::v1::transport::PrepareOutbound(
		engine, *exact, 17, 1);
	assert(exact_send.has_value());
	assert(exact_send->generation == 17);
	assert(exact_send->id == 1);

	std::size_t posts{};
	std::string posted;
	const auto post_if_valid = [&](auto&& built) {
		if (!built) return false;
		++posts;
		posted = std::move(*built);
		return true;
	};
	assert(post_if_valid(BuildLegacyOutbound("PRIVMSG", target, exact_payload)));
	assert(posts == 1);
	assert(posted.size() == maximum_legacy_wire_bytes);

	exact_payload.push_back('x');
	auto too_long = BuildLegacyOutbound("PRIVMSG", target, exact_payload);
	assert(!too_long.has_value());
	assert(too_long.error() == AdapterError::line_too_long);
	assert(!post_if_valid(BuildLegacyOutbound("PRIVMSG", target, exact_payload)));
	assert(posts == 1);

	const std::string eight_kibibytes(8U * 1024U, 'm');
	auto huge = BuildLegacyOutbound("PRIVMSG", target, eight_kibibytes);
	assert(!huge.has_value());
	assert(huge.error() == AdapterError::line_too_long);

	std::string profile = "# HeresInfo: ";
	profile.append(400, 'p');
	auto profile_line = BuildLegacyOutbound("PRIVMSG", target, profile);
	assert(profile_line.has_value());
	assert(profile_line->ends_with("\r\n"));
	const std::string profile_body(400, 'p');
	const std::array<std::string_view, 2> profile_parts{"# HeresInfo: ", profile_body};
	auto fragmented_profile = BuildLegacyOutbound(
		"PRIVMSG", target, std::span<const std::string_view>{profile_parts});
	assert(fragmented_profile == profile_line);
	profile.append(8U * 1024U, 'p');
	auto oversized_profile = BuildLegacyOutbound("PRIVMSG", target, profile);
	assert(!oversized_profile.has_value());
	assert(oversized_profile.error() == AdapterError::line_too_long);
	const std::array<std::string_view, 2> oversized_profile_parts{
		"# HeresInfo: ", eight_kibibytes};
	assert(BuildLegacyOutbound("PRIVMSG", target,
		std::span<const std::string_view>{oversized_profile_parts}).error() ==
		AdapterError::line_too_long);

	std::string annotation{"(#"};
	annotation.push_back('\x01');
	annotation.push_back('\x02');
	annotation.push_back(static_cast<char>(0x80));
	annotation += ") hello";
	auto annotated = BuildLegacyOutbound("PRIVMSG", target, annotation);
	assert(annotated.has_value());
	assert(annotated->find(annotation) != std::string::npos);
	const std::array<std::string_view, 3> annotation_parts{
		std::string_view{annotation}.substr(0, 2),
		std::string_view{annotation}.substr(2, 3),
		std::string_view{annotation}.substr(5)};
	auto fragmented_annotation = BuildLegacyOutbound(
		"PRIVMSG", target, std::span<const std::string_view>{annotation_parts});
	assert(fragmented_annotation.has_value());
	assert(fragmented_annotation == annotated);
	const std::array<std::string_view, 2> injected_annotation_parts{
		std::string_view{annotation}, "\r\nOPER root"};
	assert(BuildLegacyOutbound("PRIVMSG", target,
		std::span<const std::string_view>{injected_annotation_parts}).error() ==
		AdapterError::invalid_line);

	const std::array<std::string_view, 1> injected_target{"#ink\rJOIN #other"};
	assert(BuildLegacyOutbound("PRIVMSG", injected_target, "hello").error() ==
		AdapterError::invalid_line);
	assert(BuildLegacyOutbound("PRIVMSG\nOPER", target, "hello").error() ==
		AdapterError::invalid_line);
	assert(BuildLegacyOutbound("PRIVMSG", target, "hello\r\nOPER root").error() ==
		AdapterError::invalid_line);
	constexpr char nul_target_bytes[] = {'#', 'i', 'n', 'k', '\0', 'x'};
	const std::array<std::string_view, 1> nul_target{
		std::string_view{nul_target_bytes, sizeof(nul_target_bytes)}};
	assert(BuildLegacyOutbound("PRIVMSG", nul_target, "hello").error() ==
		AdapterError::invalid_line);
	constexpr char nul_payload_bytes[] = {'h', 'i', '\0', 'x'};
	assert(BuildLegacyOutbound("PRIVMSG", target,
		std::string_view{nul_payload_bytes, sizeof(nul_payload_bytes)}).error() ==
		AdapterError::invalid_line);

	const std::array<std::string_view, 2> mode_params{"#ink", "+o"};
	auto mode = BuildLegacyOutbound("MODE", mode_params, std::nullopt);
	assert(mode.has_value());
	assert(*mode == "MODE #ink +o\r\n");

	assert(BuildLegacyOutbound("", {}, std::nullopt).error() ==
		AdapterError::invalid_line);
	const std::string oversized_command(maximum_legacy_wire_bytes - 1U, 'A');
	assert(BuildLegacyOutbound(oversized_command, {}, std::nullopt).error() ==
		AdapterError::line_too_long);
	const std::array<std::string_view, 1> empty_parameter{""};
	assert(BuildLegacyOutbound("LIST", empty_parameter, std::nullopt).error() ==
		AdapterError::invalid_line);
	const std::array<std::string_view, 1> oversized_parameter{eight_kibibytes};
	assert(BuildLegacyOutbound("LIST", oversized_parameter, std::nullopt).error() ==
		AdapterError::line_too_long);
	const std::string maximum_command(maximum_legacy_wire_bytes - 2U, 'A');
	assert(BuildLegacyOutbound(maximum_command, {}, std::string_view{}).error() ==
		AdapterError::line_too_long);
}

void TestLegacyOutboundMicrosoftWireGrammar()
{
	using comic_chat::v1::transport::BuildLegacyOutbound;
	const std::array<std::string_view, 1> channel{"#ink"};
	assert(BuildLegacyOutbound("PART", channel, std::nullopt) ==
		std::string{"PART #ink\r\n"});
	assert(BuildLegacyOutbound("JOIN", channel, std::nullopt) ==
		std::string{"JOIN #ink\r\n"});
	const std::array<std::string_view, 1> nickname{"Tiki"};
	assert(BuildLegacyOutbound("NICK", nickname, std::nullopt) ==
		std::string{"NICK Tiki\r\n"});
	assert(BuildLegacyOutbound("PONG", nickname, std::nullopt) ==
		std::string{"PONG Tiki\r\n"});

	const std::array<std::string_view, 1> target{"Alice"};
	const std::array<std::string_view, 4> avatar{
		"#", " Appears as ", "Tiki", "."};
	assert(BuildLegacyOutbound("PRIVMSG", target,
		std::span<const std::string_view>{avatar}) ==
		std::string{"PRIVMSG Alice :# Appears as Tiki.\r\n"});
	const std::array<std::string_view, 3> profile{
		"#", " HeresInfo: ", "likes ink"};
	assert(BuildLegacyOutbound("PRIVMSG", target,
		std::span<const std::string_view>{profile}) ==
		std::string{"PRIVMSG Alice :# HeresInfo: likes ink\r\n"});

	const std::array<std::string_view, 2> kick_parameters{"#ink", "Alice"};
	assert(BuildLegacyOutbound("KICK", kick_parameters, "reason") ==
		std::string{"KICK #ink Alice :reason\r\n"});
	assert(BuildLegacyOutbound("TOPIC", channel, "new topic") ==
		std::string{"TOPIC #ink :new topic\r\n"});
	const std::array<std::string_view, 3> mode_parameters{"#ink", "+o", "Alice"};
	assert(BuildLegacyOutbound("MODE", mode_parameters, std::nullopt) ==
		std::string{"MODE #ink +o Alice\r\n"});
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
	TestCheckedLegacyOutboundBuilder();
	TestLegacyOutboundMicrosoftWireGrammar();
	TestBoundedLegacyInboundFacade();
	return 0;
}
