#ifndef COMIC_CHAT_IRCV3_EVENT_BRIDGE_H
#define COMIC_CHAT_IRCV3_EVENT_BRIDGE_H

#include "comicchat/net/connection_engine.hpp"
#include "comicchat/net/ircv3.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

struct Ircv3AdapterEvent {
	comic_chat::ircv3::Event event;
	// MessageContext carries the complete parsed message, including typed and
	// unknown tags. UI consumers must not reconstruct context from tag-stripped
	// legacy wire text.
	std::optional<comic_chat::ircv3::Message> message;
};

namespace comic_chat::legacy_ui {

enum class IrcTransportIngressPhase : std::uint8_t {
	stopped,
	connecting,
	connected,
	reconnecting,
};

enum class IrcTransportIngressAction : std::uint8_t {
	accepted,
	stale,
	out_of_order,
};

// The transport normally emits events in order, but this adapter is the final
// trust boundary before Microsoft's stateful parser. Keep generation and phase
// validation independent of MFC so malformed ordering is causally testable.
class IrcTransportIngressGate final {
public:
	void Begin(comicchat::net::GenerationId generation) noexcept
	{
		generation_ = generation;
		phase_ = generation == 0
			? IrcTransportIngressPhase::stopped
			: IrcTransportIngressPhase::connecting;
	}

	void Stop() noexcept
	{
		generation_ = 0;
		phase_ = IrcTransportIngressPhase::stopped;
	}

	[[nodiscard]] IrcTransportIngressAction Classify(
		const comicchat::net::Event& event) noexcept
	{
		if (generation_ == 0 || event.generation != generation_)
			return IrcTransportIngressAction::stale;
		return std::visit([this](const auto& body) noexcept {
			using Body = std::remove_cvref_t<decltype(body)>;
			if constexpr (std::is_same_v<Body, comicchat::net::Connected>) {
				if (phase_ != IrcTransportIngressPhase::connecting &&
					phase_ != IrcTransportIngressPhase::reconnecting)
					return IrcTransportIngressAction::out_of_order;
				phase_ = IrcTransportIngressPhase::connected;
				return IrcTransportIngressAction::accepted;
			} else if constexpr (std::is_same_v<Body, comicchat::net::BytesReceived> ||
				std::is_same_v<Body, comicchat::net::PingDue> ||
				std::is_same_v<Body, comicchat::net::SendComplete>) {
				return phase_ == IrcTransportIngressPhase::connected
					? IrcTransportIngressAction::accepted
					: IrcTransportIngressAction::out_of_order;
			} else if constexpr (std::is_same_v<Body, comicchat::net::Closed>) {
				if (phase_ == IrcTransportIngressPhase::stopped)
					return IrcTransportIngressAction::out_of_order;
				phase_ = body.retry_after > std::chrono::milliseconds::zero()
					? IrcTransportIngressPhase::reconnecting
					: IrcTransportIngressPhase::stopped;
				return IrcTransportIngressAction::accepted;
			} else {
				return phase_ == IrcTransportIngressPhase::stopped
					? IrcTransportIngressAction::out_of_order
					: IrcTransportIngressAction::accepted;
			}
		}, event.body);
	}

	[[nodiscard]] IrcTransportIngressPhase phase() const noexcept { return phase_; }

private:
	comicchat::net::GenerationId generation_{};
	IrcTransportIngressPhase phase_{IrcTransportIngressPhase::stopped};
};

inline constexpr std::size_t kIrcProtocolLinesPerWake = 512;

class IrcProtocolLineBudget final {
public:
	explicit IrcProtocolLineBudget(
		std::size_t maximum = kIrcProtocolLinesPerWake) noexcept
		: remaining_{maximum} {}

	[[nodiscard]] bool Consume(std::size_t count) noexcept
	{
		if (count > remaining_) return false;
		remaining_ -= count;
		return true;
	}

	[[nodiscard]] std::size_t remaining() const noexcept { return remaining_; }

private:
	std::size_t remaining_{};
};

enum class Ircv3UserMutationKind {
	none,
	account,
	away,
	host,
	realname,
};

// A non-owning, MFC-independent description of the only typed identity
// mutations the v2.5 model currently consumes. Views remain valid for the
// duration of the synchronous WM_COMICCHAT_IRCV3_EVENT broadcast.
struct Ircv3UserMutation {
	Ircv3UserMutationKind kind = Ircv3UserMutationKind::none;
	std::string_view nickname;
	std::string_view value;
	std::string_view secondary;
	bool active = false;
};

enum class Ircv3UserMutationResult {
	ignored,
	unknown_user,
	applied,
};

// A 353 names one member per token, so its identities resolve against many
// users at once. Report the counts instead of a single verdict: a room legibly
// holds only some of the named members.
struct Ircv3NamesIdentityResult {
	std::size_t applied{};
	std::size_t unknown_user{};
	bool ignored{};
};

enum class Ircv3StatusSeverity {
	error,
	warning,
	information,
};

struct Ircv3StatusPresentation {
	Ircv3StatusSeverity severity = Ircv3StatusSeverity::information;
	std::string text;
};

struct Ircv3ChannelRename {
	std::string_view previous;
	std::string_view current;
	std::string_view reason;
};

enum class Ircv3ChannelRenameResult {
	ignored,
	not_target,
	applied,
};

constexpr std::size_t kIrcv3LegacyNicknameMaximum = 255;
constexpr std::size_t kIrcv3LegacyIdentityPartMaximum = 255;
constexpr std::size_t kIrcv3LegacyUserValueMaximum = 512;

inline bool ValidUserMutationText(std::string_view value, std::size_t maximum) noexcept
{
	return value.size() <= maximum && value.find('\0') == std::string_view::npos;
}

inline bool ValidStandardReplyToken(std::string_view value) noexcept
{
	if (value.empty() || value.size() > 64) return false;
	return std::ranges::all_of(value, [](const unsigned char ch) {
		return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
			(ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '*';
	});
}

inline bool ValidLegacyChannelName(std::string_view value) noexcept
{
	return !value.empty() && value.size() <= kIrcv3LegacyIdentityPartMaximum &&
		value.front() != ':' && value.find('\0') == std::string_view::npos &&
		value.find_first_of(" ,\a\r\n") == std::string_view::npos;
}

inline std::optional<std::string> PrepareExplicitNamesRequest(
	bool no_implicit_names_enabled,
	std::string_view channel)
{
	if (!no_implicit_names_enabled) return std::nullopt;
	if (!ValidLegacyChannelName(channel)) return std::nullopt;
	comic_chat::ircv3::Message request;
	request.command = "NAMES";
	request.params.emplace_back(channel);
	auto wire = request.SerializeChecked(false);
	if (!wire || wire->size() > 512) return std::nullopt;
	return std::move(*wire);
}

inline std::optional<Ircv3ChannelRename> ClassifyChannelRename(
	const comic_chat::ircv3::Event& event) noexcept
{
	if (event.type != comic_chat::ircv3::EventType::ChannelRenamed ||
		!ValidLegacyChannelName(event.target) || !ValidLegacyChannelName(event.value) ||
		!ValidUserMutationText(event.detail, kIrcv3LegacyUserValueMaximum))
		return std::nullopt;
	return Ircv3ChannelRename{event.target, event.value, event.detail};
}

inline std::string FormatChannelRenameStatus(const Ircv3ChannelRename& rename)
{
	std::string result;
	result.reserve(rename.previous.size() + rename.current.size() + rename.reason.size() + 20);
	result = "[RENAME] ";
	result += rename.previous;
	result += " -> ";
	result += rename.current;
	if (!rename.reason.empty()) {
		result += ": ";
		for (const unsigned char ch : rename.reason)
			result.push_back(ch < 0x20 || ch == 0x7f ? ' ' : static_cast<char>(ch));
	}
	return result;
}

template <typename MatchesChannel, typename ApplyRename>
Ircv3ChannelRenameResult ConsumeChannelRename(
	const comic_chat::ircv3::Event& event,
	MatchesChannel&& matches_channel,
	ApplyRename&& apply_rename)
{
	const auto rename = ClassifyChannelRename(event);
	if (!rename) return Ircv3ChannelRenameResult::ignored;
	if (!std::invoke(std::forward<MatchesChannel>(matches_channel), rename->previous))
		return Ircv3ChannelRenameResult::not_target;
	std::invoke(std::forward<ApplyRename>(apply_rename), *rename);
	return Ircv3ChannelRenameResult::applied;
}

// Standard replies replace ad-hoc server notices only when their required
// human description reaches the user. Format a bounded, tag-free status line;
// the native view chooses colors using Microsoft's original status renderer.
inline std::optional<Ircv3StatusPresentation> ClassifyStandardReply(
	const comic_chat::ircv3::Event& event)
{
	if (event.type != comic_chat::ircv3::EventType::StandardReply ||
		!ValidStandardReplyToken(event.target) || !ValidStandardReplyToken(event.value) ||
		event.detail.empty() || !ValidUserMutationText(event.detail, 512))
		return std::nullopt;

	Ircv3StatusPresentation result;
	if (event.key == "FAIL") result.severity = Ircv3StatusSeverity::error;
	else if (event.key == "WARN") result.severity = Ircv3StatusSeverity::warning;
	else if (event.key == "NOTE") result.severity = Ircv3StatusSeverity::information;
	else return std::nullopt;
	result.text.reserve(event.key.size() + event.target.size() + event.value.size() +
		event.detail.size() + 8);
	result.text = '[';
	result.text += event.key;
	result.text += "] ";
	result.text += event.target;
	result.text += '/';
	result.text += event.value;
	result.text += ": ";
	for (const unsigned char ch : event.detail)
		result.text.push_back(ch < 0x20 || ch == 0x7f ? ' ' : static_cast<char>(ch));
	return result;
}

inline Ircv3UserMutation ClassifyUserMutation(
	const comic_chat::ircv3::Event& event) noexcept
{
	std::string_view nickname = event.source;
	// ACCOUNT notifications use source. The SASL 900/901 numerics instead put
	// the affected local nickname in target.
	if (event.type == comic_chat::ircv3::EventType::Account && nickname.empty())
		nickname = event.target;
	if (nickname.empty() || !ValidUserMutationText(nickname, kIrcv3LegacyNicknameMaximum) ||
		nickname.front() == '@' || nickname.front() == '>' || nickname.front() == '+')
		return {};

	switch (event.type) {
	case comic_chat::ircv3::EventType::Account:
		if (!ValidUserMutationText(event.value, kIrcv3LegacyUserValueMaximum)) return {};
		return {Ircv3UserMutationKind::account, nickname, event.value, {}, !event.value.empty()};
	case comic_chat::ircv3::EventType::Away:
		if (!ValidUserMutationText(event.value, kIrcv3LegacyUserValueMaximum)) return {};
		return {Ircv3UserMutationKind::away, nickname, event.value, {}, !event.value.empty()};
	case comic_chat::ircv3::EventType::HostChanged:
		if (event.key.empty() || event.value.empty() ||
			!ValidUserMutationText(event.key, kIrcv3LegacyIdentityPartMaximum) ||
			!ValidUserMutationText(event.value, kIrcv3LegacyIdentityPartMaximum) ||
			event.key.find('@') != std::string::npos ||
			event.value.find('@') != std::string::npos)
			return {};
		return {Ircv3UserMutationKind::host, nickname, event.key, event.value, true};
	case comic_chat::ircv3::EventType::RealnameChanged:
		if (!ValidUserMutationText(event.value, kIrcv3LegacyUserValueMaximum)) return {};
		return {Ircv3UserMutationKind::realname, nickname, event.value, {}, !event.value.empty()};
	default:
		return {};
	}
}

// Resolve through the receiving document, then mutate only that document's
// CUserInfo. Keeping lookup in the callback prevents this bridge from gaining
// any dependency on MFC or on process-global room state.
template <typename FindUser, typename ApplyMutation>
Ircv3UserMutationResult ConsumeUserMutation(
	const comic_chat::ircv3::Event& event,
	FindUser&& find_user,
	ApplyMutation&& apply_mutation)
{
	const auto mutation = ClassifyUserMutation(event);
	if (mutation.kind == Ircv3UserMutationKind::none)
		return Ircv3UserMutationResult::ignored;
	auto* user = std::invoke(std::forward<FindUser>(find_user), mutation.nickname);
	if (!user)
		return Ircv3UserMutationResult::unknown_user;
	std::invoke(std::forward<ApplyMutation>(apply_mutation), *user, mutation);
	return Ircv3UserMutationResult::applied;
}

// userhost-in-names makes each 353 token arrive as nick!user@host, but the
// legacy member model must keep receiving a bare nickname. The names normalizer
// splits the token and AdaptProtocolMessage carries the otherwise discarded
// halves in the typed context as flat nickname/user/host triples; recover them
// here as the same host mutation chghost already applies. Any corrupt triple
// rejects the whole reply rather than half-populating identities. Views alias
// the event and stay valid for the synchronous WM_COMICCHAT_IRCV3_EVENT
// broadcast.
inline std::vector<Ircv3UserMutation> ClassifyNamesIdentities(
	const comic_chat::ircv3::Event& event)
{
	if (event.type != comic_chat::ircv3::EventType::MessageContext ||
		event.key != "353" || event.context.empty() || event.context.size() % 3 != 0)
		return {};
	std::vector<Ircv3UserMutation> mutations;
	mutations.reserve(event.context.size() / 3);
	for (std::size_t index = 0; index + 2 < event.context.size(); index += 3) {
		const std::string_view nickname = event.context[index];
		const std::string_view user = event.context[index + 1];
		const std::string_view host = event.context[index + 2];
		if (nickname.empty() || user.empty() || host.empty() ||
			!ValidUserMutationText(nickname, kIrcv3LegacyNicknameMaximum) ||
			nickname.front() == '@' || nickname.front() == '>' || nickname.front() == '+' ||
			!ValidUserMutationText(user, kIrcv3LegacyIdentityPartMaximum) ||
			!ValidUserMutationText(host, kIrcv3LegacyIdentityPartMaximum) ||
			user.find('@') != std::string_view::npos ||
			host.find('@') != std::string_view::npos)
			return {};
		mutations.push_back({Ircv3UserMutationKind::host, nickname, user, host, true});
	}
	return mutations;
}

// Resolve every named member through the receiving document, exactly as
// ConsumeUserMutation does for chghost. A member the document does not hold is
// counted, never created: the 353 legacy wire alone decides room membership.
template <typename FindUser, typename ApplyMutation>
Ircv3NamesIdentityResult ConsumeNamesIdentities(
	const comic_chat::ircv3::Event& event,
	FindUser&& find_user,
	ApplyMutation&& apply_mutation)
{
	const auto mutations = ClassifyNamesIdentities(event);
	if (mutations.empty()) return {0, 0, true};
	Ircv3NamesIdentityResult result;
	for (const auto& mutation : mutations) {
		auto* user = std::invoke(find_user, mutation.nickname);
		if (!user) {
			++result.unknown_user;
			continue;
		}
		std::invoke(apply_mutation, *user, mutation);
		++result.applied;
	}
	return result;
}

struct Ircv3LegacyMessageAdaptation {
	std::optional<Ircv3AdapterEvent> typed_context;
	std::optional<std::string> legacy_wire;
	bool rejected_legacy_shape{};
};

// The historical parser uses a one-based args array (args[0] is the command)
// and several release-build handlers dereference mandatory arguments after an
// ASSERT-only check. Reject malformed typed messages before flattening them;
// this keeps Microsoft's valid wire behavior without preserving its null-
// dereference trust boundary.
inline bool HasSafeLegacyDispatchShape(const comic_chat::ircv3::Message& message) noexcept
{
	const auto has_nonempty = [&message](const std::size_t index) {
		return index < message.params.size() && !message.params[index].empty();
	};
	if (message.command == "JOIN")
		return has_nonempty(0) &&
			(message.params.size() == 1 || message.params.size() == 3);
	if (message.command == "PART" || message.command == "KILL" ||
		message.command == "001")
		return has_nonempty(0);
	if (message.command == "WHISPER")
		return has_nonempty(0) && has_nonempty(1) && message.params.size() >= 3;
	if (message.command == "353")
		return message.params.size() == 4 && has_nonempty(2) && has_nonempty(3);
	return true;
}

inline bool LegacyHandlerNeedsTrailingParameter(const comic_chat::ircv3::Message& message) noexcept
{
	if (message.params.empty()) return false;
	if (message.command == "NICK" || message.command == "JOIN" ||
		message.command == "INVITE" || message.command == "PRIVMSG" ||
		message.command == "NOTICE" || message.command == "ERROR" ||
		message.command == "353") return true;
	if ((message.command == "KICK" || message.command == "WHISPER" ||
		 message.command == "DATA" || message.command == "PROP") &&
		message.params.size() >= 3) return true;
	if (message.command == "TOPIC" && message.params.size() >= 2) return true;
	if (message.command == "001" && message.params.size() >= 2) return true;
	return false;
}

// When identities is non-null, the user@host halves that the legacy nickname
// column cannot carry are appended as flat nickname/user/host triples, but only
// once the whole reply normalizes. A rejected reply leaves identities untouched
// rather than half-written, and a token whose hostmask is absent, incomplete, or
// oversize still keeps its member: the identity is dropped, never truncated.
inline std::optional<comic_chat::ircv3::Message> NormalizeLegacyNamesReply(
	const comic_chat::ircv3::Message& message,
	std::string_view prefix_token,
	std::vector<std::string>* identities = nullptr)
{
	if (message.command != "353") return message;
	if (!HasSafeLegacyDispatchShape(message) || prefix_token.size() > 66 ||
		prefix_token.empty() || prefix_token.front() != '(')
		return std::nullopt;
	const auto close = prefix_token.find(')');
	if (close == std::string_view::npos || close <= 1 || close + 1 >= prefix_token.size())
		return std::nullopt;
	const auto modes = prefix_token.substr(1, close - 1);
	const auto symbols = prefix_token.substr(close + 1);
	if (modes.size() != symbols.size() || modes.size() > 32)
		return std::nullopt;
	for (std::size_t index = 0; index < symbols.size(); ++index) {
		const unsigned char mode = modes[index];
		const unsigned char symbol = symbols[index];
		if (mode <= 0x20U || mode == 0x7fU || symbol <= 0x20U || symbol == 0x7fU ||
			modes.find(modes[index]) != index || symbols.find(symbols[index]) != index)
			return std::nullopt;
	}

	auto legacy_status = [](const char mode, const char symbol) noexcept -> char {
		// Microsoft's member model has owner, operator, spectator, and voice.
		// Collapse higher ranked server roles to the closest representable role,
		// while preserving IRCX's original literal status symbols.
		if (symbol == '.') return '.';
		if (symbol == '>') return '>';
		if (mode == 'q') return '.';
		if (mode == 'a' || mode == 'o' || mode == 'h' || symbol == '@') return '@';
		if (mode == 'v' || symbol == '+') return '+';
		return '\0';
	};

	std::string names;
	std::vector<std::string> collected;
	const std::string_view source = message.params.back();
	names.reserve(source.size());
	for (std::size_t cursor = 0; cursor < source.size();) {
		while (cursor < source.size() && source[cursor] == ' ') ++cursor;
		if (cursor == source.size()) break;
		const auto end = source.find(' ', cursor);
		const auto token = source.substr(cursor,
			end == std::string_view::npos ? source.size() - cursor : end - cursor);

		std::size_t nickname_start = 0;
		char selected_status = '\0';
		while (nickname_start < token.size()) {
			const auto status = symbols.find(token[nickname_start]);
			if (status == std::string_view::npos) break;
			if (selected_status == '\0')
				selected_status = legacy_status(modes[status], symbols[status]);
			++nickname_start;
		}
		const auto hostmask = token.find('!', nickname_start);
		const auto nickname_end = hostmask == std::string_view::npos ? token.size() : hostmask;
		const auto nickname = token.substr(nickname_start, nickname_end - nickname_start);
		if (nickname.empty() || nickname.size() > kIrcv3LegacyNicknameMaximum ||
			nickname.find_first_of(" ,\a\r\n\0") != std::string_view::npos)
			return std::nullopt;

		// The '!' already located the nickname boundary; keep the remainder
		// instead of discarding it, applying the same bounds chghost enforces.
		if (identities && hostmask != std::string_view::npos) {
			const auto mask = token.substr(hostmask + 1);
			const auto separator = mask.find('@');
			if (separator != std::string_view::npos) {
				const auto user = mask.substr(0, separator);
				const auto host = mask.substr(separator + 1);
				if (!user.empty() && !host.empty() &&
					ValidUserMutationText(user, kIrcv3LegacyIdentityPartMaximum) &&
					ValidUserMutationText(host, kIrcv3LegacyIdentityPartMaximum) &&
					host.find('@') == std::string_view::npos) {
					collected.emplace_back(nickname);
					collected.emplace_back(user);
					collected.emplace_back(host);
				}
			}
		}

		if (!names.empty()) names.push_back(' ');
		if (selected_status != '\0') names.push_back(selected_status);
		names.append(nickname);
		if (end == std::string_view::npos) break;
		cursor = end + 1;
	}
	if (names.empty()) return std::nullopt;

	if (identities)
		identities->insert(identities->end(),
			std::make_move_iterator(collected.begin()),
			std::make_move_iterator(collected.end()));
	auto normalized = message;
	normalized.params.back() = std::move(names);
	return normalized;
}

inline std::optional<std::string> PrepareLegacyProtocolWire(
	const comic_chat::ircv3::Message& message,
	std::string_view prefix_token = "(ov)@+")
{
	if (!HasSafeLegacyDispatchShape(message)) return std::nullopt;
	const comic_chat::ircv3::Message* legacy_message = &message;
	comic_chat::ircv3::Message normalized;
	if (message.command == "JOIN" && message.params.size() == 3) {
		// extended-join adds account and realname, but Microsoft's handler uses
		// the single trailing JOIN field as the channel key. Preserve the complete
		// message for typed consumers and flatten only the channel into the old
		// parser; otherwise the realname becomes LookupDoc()'s input.
		normalized = message;
		normalized.params.resize(1);
		legacy_message = &normalized;
	} else if (message.command == "353") {
		auto names = NormalizeLegacyNamesReply(message, prefix_token);
		if (!names) return std::nullopt;
		normalized = std::move(*names);
		legacy_message = &normalized;
	}
	auto wire = legacy_message->SerializeChecked(false);
	if (!wire) return std::nullopt;
	if (LegacyHandlerNeedsTrailingParameter(*legacy_message)) {
		const auto final_start = wire->size() - 2 - legacy_message->params.back().size();
		if (final_start == 0 || (*wire)[final_start - 1] != ':') {
			wire->insert(final_start, 1, ':');
			if (wire->size() > 512) return std::nullopt;
		}
	}
	return std::move(*wire);
}

// Preserve the complete parsed message for typed consumers before flattening
// ordinary IRC into the historical command parser. TAGMSG is tag-only protocol
// metadata, including when a server has stripped every tag, and must never be
// presented to the legacy unknown-command/history path.
inline Ircv3LegacyMessageAdaptation AdaptProtocolMessage(
	const comic_chat::ircv3::Message& message,
	std::string_view prefix_token = "(ov)@+")
{
	Ircv3LegacyMessageAdaptation result;
	const bool typed_only = message.command == "TAGMSG";
	const bool extended_join = message.command == "JOIN" && message.params.size() == 3;
	// userhost-in-names: the hostmask is stripped before the legacy parser sees
	// the reply, so split it out here and let the typed context carry it. This
	// normalizes a second time inside PrepareLegacyProtocolWire, which 353's
	// join-time-only arrival makes cheaper than widening that helper's contract.
	std::vector<std::string> names_identities;
	if (message.command == "353")
		(void)NormalizeLegacyNamesReply(message, prefix_token, &names_identities);
	if (!message.tags.empty() || typed_only || extended_join || !names_identities.empty()) {
		comic_chat::ircv3::Event context;
		context.type = comic_chat::ircv3::EventType::MessageContext;
		context.source = message.prefix ? *message.prefix : std::string{};
		context.target = message.params.empty() ? std::string{} : message.params.front();
		context.key = message.command;
		context.context = std::move(names_identities);
		result.typed_context = Ircv3AdapterEvent{std::move(context), message};
	}
	if (!typed_only) {
		if (!HasSafeLegacyDispatchShape(message)) {
			result.rejected_legacy_shape = true;
		} else {
			result.legacy_wire = PrepareLegacyProtocolWire(message, prefix_token);
		}
	}
	return result;
}

constexpr std::size_t kIrcv3UiDrainBatch = 128;
constexpr std::size_t kIrcv3UiDrainMaximum = 512;

struct Ircv3UiDrainResult {
	std::size_t drained{};
	std::size_t delivered{};
	std::size_t rejected{};
	std::uint64_t dropped_before_delivery{};
};

// Source contract:
//   vector<Ircv3AdapterEvent> PollIrcv3Events(size_t maximum)
//   uint64_t DroppedIrcv3Events() const
// Sink contract:
//   void(Ircv3AdapterEvent) -- invoked synchronously on the UI thread.
// The helper bounds one event-loop turn while forwarding the complete event
// and optional tagged MessageContext without flattening either representation.
template <typename Source, typename Sink>
Ircv3UiDrainResult DrainIrcv3EventsForUi(
	Source& source,
	Sink&& sink,
	std::size_t maximum = kIrcv3UiDrainMaximum)
{
	Ircv3UiDrainResult result;
	maximum = (std::min)(maximum, kIrcv3UiDrainMaximum);
	while (result.drained < maximum) {
		const auto requested = (std::min)(kIrcv3UiDrainBatch, maximum - result.drained);
		try {
			auto events = source.PollIrcv3Events(requested);
			if (events.empty()) break;
			result.drained += events.size();
			for (auto& event : events) {
				try {
					std::invoke(sink, std::move(event));
					++result.delivered;
				} catch (...) {
					// One faulty view must not block later typed events in the same
					// bounded UI turn.
					++result.rejected;
				}
			}
			if (events.size() < requested) break;
		} catch (...) {
			// Polling allocates its return vector. Treat an allocation failure as
			// a rejected UI handoff without unwinding across the window proc.
			++result.rejected;
			break;
		}
	}
	try {
		result.dropped_before_delivery = source.DroppedIrcv3Events();
	} catch (...) {
		++result.rejected;
	}
	return result;
}

} // namespace comic_chat::legacy_ui

#endif
