// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#ifndef COMIC_CHAT_V1_TRANSPORT_ADAPTER_H
#define COMIC_CHAT_V1_TRANSPORT_ADAPTER_H

#include "comicchat/net/connection_engine.hpp"
#include "comicchat/net/ircv3.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <mbedtls/platform_util.h>

namespace comic_chat::v1::transport {

inline constexpr std::size_t maximum_irc_wire_bytes = 8191U + 512U;
inline constexpr std::size_t maximum_legacy_wire_bytes = 512U;
inline constexpr std::size_t maximum_legacy_prefix_component_bytes = 49U;
inline constexpr std::size_t maximum_legacy_channel_bytes = 255U;
inline constexpr std::size_t maximum_isupport_prefix_bytes = 66U;
inline constexpr std::size_t maximum_isupport_prefix_statuses = 32U;
inline constexpr std::size_t maximum_protocol_lines_per_ui_wake = 512U;

enum class AdapterError : std::uint8_t {
	not_open,
	invalid_line,
	line_too_long,
	allocation_failed,
	transport_error,
};

// One instance is created for each drained Windows UI notification and shared
// by every BytesReceived event in that drain. This bounds parser and legacy UI
// work across event-batch boundaries rather than merely bounding each socket
// read in isolation.
class ProtocolLineBudget final {
public:
	explicit ProtocolLineBudget(
		std::size_t maximum = maximum_protocol_lines_per_ui_wake) noexcept
		: remaining_{maximum} {}

	[[nodiscard]] bool Consume(const std::size_t count) noexcept
	{
		if (count > remaining_) return false;
		remaining_ -= count;
		return true;
	}

	[[nodiscard]] std::size_t remaining() const noexcept { return remaining_; }

private:
	std::size_t remaining_{};
};

namespace detail {

// Builds the exact Microsoft-era IRC wire shape without ever formatting into a
// fixed buffer. The optional span represents one trailing field assembled from
// zero or more fragments, avoiding a second allocation for comic annotations,
// profiles, and other historical compound payloads.
[[nodiscard]] inline std::expected<std::string, AdapterError> BuildLegacyOutboundParts(
	std::string_view command,
	std::span<const std::string_view> middle_parameters,
	std::optional<std::span<const std::string_view>> trailing_fragments)
{
	if (command.empty()) return std::unexpected(AdapterError::invalid_line);
	if (command.size() > maximum_legacy_wire_bytes - 2U)
		return std::unexpected(AdapterError::line_too_long);
	for (const unsigned char byte : command) {
		if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
			(byte >= '0' && byte <= '9'))) {
			return std::unexpected(AdapterError::invalid_line);
		}
	}

	std::size_t wire_bytes = command.size() + 2U; // terminating CRLF
	for (const auto parameter : middle_parameters) {
		if (parameter.empty() || parameter.front() == ':')
			return std::unexpected(AdapterError::invalid_line);
		if (wire_bytes >= maximum_legacy_wire_bytes ||
			parameter.size() > maximum_legacy_wire_bytes - wire_bytes - 1U)
			return std::unexpected(AdapterError::line_too_long);
		for (const unsigned char byte : parameter) {
			if (byte <= 0x20U || byte == 0x7fU)
				return std::unexpected(AdapterError::invalid_line);
		}
		wire_bytes += 1U + parameter.size();
	}
	if (trailing_fragments) {
		if (wire_bytes > maximum_legacy_wire_bytes - 2U)
			return std::unexpected(AdapterError::line_too_long);
		wire_bytes += 2U;
		for (const auto fragment : *trailing_fragments) {
			if (fragment.size() > maximum_legacy_wire_bytes - wire_bytes)
				return std::unexpected(AdapterError::line_too_long);
			for (const char byte : fragment) {
				if (byte == '\0' || byte == '\r' || byte == '\n')
					return std::unexpected(AdapterError::invalid_line);
			}
			wire_bytes += fragment.size();
		}
	}

	try {
		std::string wire;
		wire.reserve(wire_bytes);
		wire.append(command);
		for (const auto parameter : middle_parameters) {
			wire.push_back(' ');
			wire.append(parameter);
		}
		if (trailing_fragments) {
			wire.append(" :");
			for (const auto fragment : *trailing_fragments)
				wire.append(fragment);
		}
		wire.append("\r\n");
		return wire;
	} catch (...) {
		return std::unexpected(AdapterError::allocation_failed);
	}
}

} // namespace detail

// Middle parameters remain space-delimited tokens, while the optional final
// value is emitted as the traditional ` :trailing` field. Comic annotations
// and CTCP SOH bytes are data and remain intact; only bytes that can terminate
// or split the IRC record are rejected.
[[nodiscard]] inline std::expected<std::string, AdapterError> BuildLegacyOutbound(
	std::string_view command,
	std::span<const std::string_view> middle_parameters,
	std::optional<std::string_view> trailing)
{
	if (!trailing)
		return detail::BuildLegacyOutboundParts(command, middle_parameters, std::nullopt);
	const std::string_view fragment = *trailing;
	return detail::BuildLegacyOutboundParts(
		command, middle_parameters, std::span<const std::string_view>{&fragment, 1U});
}

// Fragmented trailing data is emitted as one IRC trailing field. This overload
// lets fixed legacy prefixes and existing CString payloads share the builder's
// single bounded allocation without concatenating into a temporary string.
[[nodiscard]] inline std::expected<std::string, AdapterError> BuildLegacyOutbound(
	std::string_view command,
	std::span<const std::string_view> middle_parameters,
	std::span<const std::string_view> trailing_fragments)
{
	return detail::BuildLegacyOutboundParts(command, middle_parameters, trailing_fragments);
}

// Microsoft's v1 parser stores nick, user, and host components in independent
// 50-byte buffers and assumes the traditional 512-byte IRC framing limit.
// Validate the typed message before recreating that legacy wire shape; silent
// truncation would alias identities and is therefore not acceptable.
[[nodiscard]] inline bool LegacyPrefixFits(std::string_view prefix) noexcept
{
	const auto nickname_end = prefix.find_first_of("!@");
	const auto nickname_size = nickname_end == std::string_view::npos
		? prefix.size() : nickname_end;
	if (nickname_size > maximum_legacy_prefix_component_bytes)
		return false;
	if (nickname_end == std::string_view::npos)
		return true;

	std::size_t host_start = nickname_end + 1;
	if (prefix[nickname_end] == '!') {
		const auto host_separator = prefix.find('@', host_start);
		const auto user_size = host_separator == std::string_view::npos
			? prefix.size() - host_start : host_separator - host_start;
		if (user_size > maximum_legacy_prefix_component_bytes)
			return false;
		if (host_separator == std::string_view::npos)
			return true;
		host_start = host_separator + 1;
	}
	return prefix.size() - host_start <= maximum_legacy_prefix_component_bytes;
}

[[nodiscard]] inline bool ValidLegacyChannelName(std::string_view value) noexcept
{
	if (value.empty() || value.size() > maximum_legacy_channel_bytes ||
		value.front() == ':') return false;
	for (const unsigned char byte : value) {
		if (byte <= 0x20U || byte == 0x7fU || byte == ',') return false;
	}
	return true;
}

// no-implicit-names must query the channel the server actually placed us in,
// not the channel we asked to join. A server-side forward (Libera's +f, numeric
// 470) lands us in a different channel and echoes "JOIN <other>", so the
// configured channel would query a room we are not in -- or, when empty, drop
// the connection. Recover the channel from the JOIN's own parameters instead,
// accepting both the ordinary "JOIN #chan" and trailing ":#chan" forms. The
// result is still validated by PrepareExplicitNamesRequest before use.
[[nodiscard]] inline std::string_view LegacyJoinChannel(std::string_view join_params) noexcept
{
	std::size_t start = 0;
	while (start < join_params.size() && join_params[start] == ' ') ++start;
	if (start < join_params.size() && join_params[start] == ':') ++start;
	std::size_t end = start;
	while (end < join_params.size() && join_params[end] != ' ' &&
		join_params[end] != '\r' && join_params[end] != '\n')
		++end;
	return join_params.substr(start, end - start);
}

// no-implicit-names suppresses only the server's automatic reply. Build the
// one explicit query the Microsoft UI needs after its own JOIN, while keeping
// the disabled path allocation-free and incapable of duplicating a request.
[[nodiscard]] inline std::optional<std::string> PrepareExplicitNamesRequest(
	const bool no_implicit_names_enabled,
	std::string_view channel)
{
	if (!no_implicit_names_enabled || !ValidLegacyChannelName(channel))
		return std::nullopt;
	try {
		comic_chat::ircv3::Message request;
		request.command = "NAMES";
		request.params.emplace_back(channel);
		auto wire = request.SerializeChecked(false);
		if (!wire || wire->size() > maximum_legacy_wire_bytes)
			return std::nullopt;
		return std::move(*wire);
	} catch (...) {
		return std::nullopt;
	}
}

namespace detail {

struct PrefixMapping final {
	std::string_view modes;
	std::string_view symbols;
};

[[nodiscard]] inline std::optional<PrefixMapping> ParsePrefixMapping(
	std::string_view token) noexcept
{
	if (token.empty() || token.size() > maximum_isupport_prefix_bytes ||
		token.front() != '(') return std::nullopt;
	const auto close = token.find(')');
	if (close == std::string_view::npos || close <= 1U || close + 1U >= token.size())
		return std::nullopt;
	const auto modes = token.substr(1U, close - 1U);
	const auto symbols = token.substr(close + 1U);
	if (modes.size() != symbols.size() ||
		modes.size() > maximum_isupport_prefix_statuses) return std::nullopt;
	for (std::size_t index = 0; index < modes.size(); ++index) {
		const auto mode = static_cast<unsigned char>(modes[index]);
		const auto symbol = static_cast<unsigned char>(symbols[index]);
		if (mode <= 0x20U || mode == 0x7fU || symbol <= 0x20U || symbol == 0x7fU ||
			modes.find(modes[index]) != index || symbols.find(symbols[index]) != index)
			return std::nullopt;
	}
	return PrefixMapping{modes, symbols};
}

[[nodiscard]] inline bool ValidLegacyMiddleParameter(std::string_view value) noexcept
{
	if (value.empty() || value.size() > maximum_legacy_channel_bytes ||
		value.front() == ':') return false;
	for (const unsigned char byte : value) {
		if (byte <= 0x20U || byte == 0x7fU) return false;
	}
	return true;
}

[[nodiscard]] inline bool ValidExtendedJoinShape(
	const comic_chat::ircv3::Message& message) noexcept
{
	if (!message.prefix || message.command != "JOIN" ||
		(message.params.size() != 1U && message.params.size() != 3U) ||
		!ValidLegacyChannelName(message.params.front())) return false;
	if (message.params.size() == 1U) return true;
	if (!ValidLegacyMiddleParameter(message.params[1]) ||
		message.params[2].size() > maximum_irc_wire_bytes) return false;
	for (const char byte : message.params[2]) {
		if (byte == '\0' || byte == '\r' || byte == '\n') return false;
	}
	return true;
}

[[nodiscard]] inline bool ValidLegacyNickname(std::string_view nickname) noexcept
{
	if (nickname.empty() ||
		nickname.size() > maximum_legacy_prefix_component_bytes) return false;
	for (const unsigned char byte : nickname) {
		if (byte <= 0x20U || byte == 0x7fU || byte == ',') return false;
	}
	return true;
}

[[nodiscard]] inline std::expected<comic_chat::ircv3::Message, AdapterError>
NormalizeLegacyNamesReply(
	const comic_chat::ircv3::Message& message,
	std::string_view prefix_token)
{
	if (!message.prefix || message.command != "353" || message.params.size() != 4U ||
		!ValidLegacyChannelName(message.params[2]) || message.params[3].empty())
		return std::unexpected(AdapterError::invalid_line);
	const auto mapping = ParsePrefixMapping(prefix_token);
	if (!mapping) return std::unexpected(AdapterError::invalid_line);

	std::string names;
	const std::string_view source = message.params[3];
	names.reserve(source.size());
	for (std::size_t cursor = 0; cursor < source.size();) {
		while (cursor < source.size() && source[cursor] == ' ') ++cursor;
		if (cursor == source.size()) break;
		const auto end = source.find(' ', cursor);
		const auto token = source.substr(cursor,
			end == std::string_view::npos ? source.size() - cursor : end - cursor);

		std::size_t nickname_start{};
		bool operator_role{};
		while (nickname_start < token.size()) {
			const auto status = mapping->symbols.find(token[nickname_start]);
			if (status == std::string_view::npos) break;
			const char mode = mapping->modes[status];
			operator_role = operator_role || mode == 'q' || mode == 'a' ||
				mode == 'o' || mode == 'h';
			++nickname_start;
		}
		const auto hostmask = token.find('!', nickname_start);
		const auto nickname_end = hostmask == std::string_view::npos
			? token.size() : hostmask;
		const auto nickname = token.substr(nickname_start, nickname_end - nickname_start);
		if (!ValidLegacyNickname(nickname))
			return std::unexpected(AdapterError::invalid_line);

		if (!names.empty()) names.push_back(' ');
		if (operator_role) names.push_back('@');
		names.append(nickname);
		if (end == std::string_view::npos) break;
		cursor = end + 1U;
	}
	if (names.empty()) return std::unexpected(AdapterError::invalid_line);

	auto normalized = message;
	normalized.params.back() = std::move(names);
	return normalized;
}

} // namespace detail

[[nodiscard]] inline std::expected<std::string, AdapterError> PrepareLegacyInbound(
	const comic_chat::ircv3::Message& message,
	std::string_view prefix_token = "(ov)@+")
{
	try {
		if (message.prefix && !LegacyPrefixFits(*message.prefix))
			return std::unexpected(AdapterError::invalid_line);

		const comic_chat::ircv3::Message* legacy_message = &message;
		comic_chat::ircv3::Message normalized;
		if (message.command == "JOIN") {
			if (!detail::ValidExtendedJoinShape(message))
				return std::unexpected(AdapterError::invalid_line);
			if (message.params.size() == 3U) {
				normalized = message;
				normalized.params.resize(1U);
				legacy_message = &normalized;
			}
		} else if (message.command == "353") {
			auto names = detail::NormalizeLegacyNamesReply(message, prefix_token);
			if (!names) return std::unexpected(names.error());
			normalized = std::move(*names);
			legacy_message = &normalized;
		}

		auto wire = legacy_message->SerializeChecked(false);
		if (!wire) return std::unexpected(AdapterError::invalid_line);
		if (wire->size() > maximum_legacy_wire_bytes)
			return std::unexpected(AdapterError::line_too_long);
		const bool needs_legacy_trailing = !legacy_message->params.empty() &&
			(legacy_message->command == "PRIVMSG" || legacy_message->command == "NICK" ||
			 legacy_message->command == "JOIN" || legacy_message->command == "319" ||
			 legacy_message->command == "322" || legacy_message->command == "353" ||
			 legacy_message->command == "ERROR" ||
			 (legacy_message->command == "KICK" && legacy_message->params.size() >= 3U));
		if (needs_legacy_trailing) {
			const auto final_start = wire->size() - 2U - legacy_message->params.back().size();
			if (final_start == 0U || (*wire)[final_start - 1U] != ':')
				wire->insert(final_start, 1U, ':');
			if (wire->size() > maximum_legacy_wire_bytes)
				return std::unexpected(AdapterError::line_too_long);
		}
		return std::move(*wire);
	} catch (...) {
		return std::unexpected(AdapterError::allocation_failed);
	}
}

enum class SessionPhase : std::uint8_t {
	stopped,
	starting,
	connected,
	reconnecting,
};

enum class EventAction : std::uint8_t {
	stale,
	out_of_order,
	state_changed,
	connected,
	bytes_received,
	send_complete,
	ping_due,
	connect_failed,
	disconnected,
	diagnostic,
};

// UI-thread-owned generation/order policy. ConnectionEngine owns all network
// concurrency; this gate makes the adapter's MFC-facing effects deterministic
// and prevents a queued event from a previous server session reaching the UI.
class SessionGate final {
public:
	void Begin(comicchat::net::GenerationId generation) noexcept
	{
		generation_ = generation;
		phase_ = generation == 0 ? SessionPhase::stopped : SessionPhase::starting;
	}

	void Stop() noexcept
	{
		generation_ = 0;
		phase_ = SessionPhase::stopped;
	}

	[[nodiscard]] EventAction Classify(const comicchat::net::Event& event) noexcept
	{
		if (generation_ == 0 || event.generation != generation_)
			return EventAction::stale;

		return std::visit([this](const auto& body) noexcept {
			using Body = std::remove_cvref_t<decltype(body)>;
			if constexpr (std::is_same_v<Body, comicchat::net::Connected>) {
				if (phase_ != SessionPhase::starting && phase_ != SessionPhase::reconnecting)
					return EventAction::out_of_order;
				phase_ = SessionPhase::connected;
				return EventAction::connected;
			} else if constexpr (std::is_same_v<Body, comicchat::net::BytesReceived>) {
				return phase_ == SessionPhase::connected
					? EventAction::bytes_received : EventAction::out_of_order;
			} else if constexpr (std::is_same_v<Body, comicchat::net::PingDue>) {
				return phase_ == SessionPhase::connected
					? EventAction::ping_due : EventAction::out_of_order;
			} else if constexpr (std::is_same_v<Body, comicchat::net::SendComplete>) {
				return phase_ == SessionPhase::connected
					? EventAction::send_complete : EventAction::out_of_order;
			} else if constexpr (std::is_same_v<Body, comicchat::net::Closed>) {
				if (phase_ == SessionPhase::stopped)
					return EventAction::out_of_order;
				const bool failed_before_connect = phase_ == SessionPhase::starting;
				phase_ = body.retry_after > std::chrono::milliseconds::zero()
					? SessionPhase::reconnecting : SessionPhase::stopped;
				return failed_before_connect
					? EventAction::connect_failed : EventAction::disconnected;
			} else if constexpr (std::is_same_v<Body, comicchat::net::StateChanged>) {
				return phase_ == SessionPhase::stopped
					? EventAction::out_of_order : EventAction::state_changed;
			} else {
				return phase_ == SessionPhase::stopped
					? EventAction::out_of_order : EventAction::diagnostic;
			}
		}, event.body);
	}

	[[nodiscard]] comicchat::net::GenerationId generation() const noexcept
	{
		return generation_;
	}

	[[nodiscard]] SessionPhase phase() const noexcept { return phase_; }
	[[nodiscard]] bool active() const noexcept { return generation_ != 0; }

private:
	comicchat::net::GenerationId generation_{};
	SessionPhase phase_{SessionPhase::stopped};
};

// Shared between the UI thread and ConnectionEngine's notifier. Only a cookie
// and a coalescing bit cross threads; HWND ownership remains in the MFC shell.
class WakeupGate final {
public:
	void Reset(std::uint64_t cookie) noexcept
	{
		pending_.store(false, std::memory_order_release);
		cookie_.store(cookie, std::memory_order_release);
	}

	void Disable() noexcept
	{
		cookie_.store(0, std::memory_order_release);
		pending_.store(false, std::memory_order_release);
	}

	[[nodiscard]] bool TryMarkPending(std::uint64_t cookie) noexcept
	{
		if (cookie == 0 || cookie_.load(std::memory_order_acquire) != cookie)
			return false;
		if (pending_.exchange(true, std::memory_order_acq_rel))
			return false;
		if (cookie_.load(std::memory_order_acquire) == cookie)
			return true;
		pending_.store(false, std::memory_order_release);
		return false;
	}

	void CancelPending(std::uint64_t cookie) noexcept
	{
		if (cookie_.load(std::memory_order_acquire) == cookie)
			pending_.store(false, std::memory_order_release);
	}

	[[nodiscard]] bool BeginDrain(std::uint64_t cookie) noexcept
	{
		if (cookie == 0 || cookie_.load(std::memory_order_acquire) != cookie)
			return false;
		pending_.store(false, std::memory_order_release);
		return cookie_.load(std::memory_order_acquire) == cookie;
	}

	[[nodiscard]] std::uint64_t cookie() const noexcept
	{
		return cookie_.load(std::memory_order_acquire);
	}

	[[nodiscard]] bool pending() const noexcept
	{
		return pending_.load(std::memory_order_acquire);
	}

private:
	std::atomic<std::uint64_t> cookie_{};
	std::atomic<bool> pending_{};
};

struct OutgoingClassification final {
	comicchat::net::Priority priority{comicchat::net::Priority::bulk};
	bool sensitive{true};
	std::string_view target;
};

[[nodiscard]] inline bool AsciiEqual(std::string_view left, std::string_view right) noexcept
{
	if (left.size() != right.size()) return false;
	for (std::size_t index = 0; index < left.size(); ++index) {
		const auto upper = [](unsigned char byte) noexcept {
			return byte >= 'a' && byte <= 'z'
				? static_cast<unsigned char>(byte - ('a' - 'A')) : byte;
		};
		if (upper(static_cast<unsigned char>(left[index])) !=
			upper(static_cast<unsigned char>(right[index]))) return false;
	}
	return true;
}

[[nodiscard]] inline OutgoingClassification ClassifyOutgoing(std::string_view wire) noexcept
{
	while (!wire.empty() && (wire.back() == '\r' || wire.back() == '\n'))
		wire.remove_suffix(1);
	std::size_t cursor{};
	if (cursor < wire.size() && wire[cursor] == '@') {
		const auto end = wire.find(' ', cursor);
		if (end == std::string_view::npos) return {};
		cursor = end + 1;
	}
	while (cursor < wire.size() && wire[cursor] == ' ') ++cursor;
	if (cursor < wire.size() && wire[cursor] == ':') {
		const auto end = wire.find(' ', cursor);
		if (end == std::string_view::npos) return {};
		cursor = end + 1;
	}
	while (cursor < wire.size() && wire[cursor] == ' ') ++cursor;
	const auto command_end = wire.find(' ', cursor);
	const auto command = wire.substr(cursor, command_end - cursor);
	if (command.empty()) return {};

	OutgoingClassification result;
	result.sensitive = AsciiEqual(command, "AUTHENTICATE") || AsciiEqual(command, "PASS") ||
		AsciiEqual(command, "AUTH") || AsciiEqual(command, "OPER") ||
		AsciiEqual(command, "REGISTER") || AsciiEqual(command, "VERIFY");
	if (AsciiEqual(command, "AUTHENTICATE") || AsciiEqual(command, "PASS"))
		result.priority = comicchat::net::Priority::authentication;
	else if (AsciiEqual(command, "PING") || AsciiEqual(command, "PONG"))
		result.priority = comicchat::net::Priority::pong;
	else if (AsciiEqual(command, "CAP") || AsciiEqual(command, "NICK") ||
		AsciiEqual(command, "USER") || AsciiEqual(command, "QUIT") ||
		AsciiEqual(command, "MODE"))
		result.priority = comicchat::net::Priority::control;
	else if (AsciiEqual(command, "PRIVMSG") || AsciiEqual(command, "NOTICE"))
		result.priority = comicchat::net::Priority::chat;

	if ((AsciiEqual(command, "PRIVMSG") || AsciiEqual(command, "NOTICE") ||
		AsciiEqual(command, "TAGMSG")) && command_end != std::string_view::npos) {
		cursor = command_end + 1;
		while (cursor < wire.size() && wire[cursor] == ' ') ++cursor;
		const auto target_end = wire.find(' ', cursor);
		result.target = wire.substr(cursor, target_end - cursor);
	}
	return result;
}

inline void SecureClear(std::string* value) noexcept
{
	if (!value) return;
	if (!value->empty()) mbedtls_platform_zeroize(value->data(), value->size());
	value->clear();
}

[[nodiscard]] inline std::expected<comicchat::net::Send, AdapterError> PrepareOutbound(
	comic_chat::ircv3::Engine& engine,
	std::string_view wire,
	comicchat::net::GenerationId generation,
	comicchat::net::SendId id)
{
	if (generation == 0) return std::unexpected(AdapterError::not_open);
	if (wire.size() > maximum_irc_wire_bytes)
		return std::unexpected(AdapterError::line_too_long);
	auto prepared = engine.PrepareOutgoingChecked(wire);
	if (!prepared) return std::unexpected(AdapterError::invalid_line);
	const auto classification = ClassifyOutgoing(*prepared);
	struct Wipe final {
		std::string* value;
		bool sensitive;
		~Wipe() { if (sensitive) SecureClear(value); }
	} wipe{&*prepared, classification.sensitive};

	comicchat::net::Send command;
	command.generation = generation;
	command.id = id;
	command.priority = classification.priority;
	command.sensitive = classification.sensitive;
	try {
		command.target.assign(classification.target);
		command.bytes.reserve(prepared->size());
		for (const char byte : *prepared)
			command.bytes.push_back(
				static_cast<std::byte>(static_cast<unsigned char>(byte)));
	} catch (...) {
		return std::unexpected(AdapterError::allocation_failed);
	}
	return command;
}

} // namespace comic_chat::v1::transport

#endif // COMIC_CHAT_V1_TRANSPORT_ADAPTER_H
