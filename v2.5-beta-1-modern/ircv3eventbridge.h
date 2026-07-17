#ifndef COMIC_CHAT_IRCV3_EVENT_BRIDGE_H
#define COMIC_CHAT_IRCV3_EVENT_BRIDGE_H

#include "comicchat/net/ircv3.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

struct Ircv3AdapterEvent {
	comic_chat::ircv3::Event event;
	// MessageContext carries the complete parsed message, including typed and
	// unknown tags. UI consumers must not reconstruct context from tag-stripped
	// legacy wire text.
	std::optional<comic_chat::ircv3::Message> message;
};

namespace comic_chat::legacy_ui {

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

constexpr std::size_t kIrcv3LegacyNicknameMaximum = 255;
constexpr std::size_t kIrcv3LegacyIdentityPartMaximum = 255;
constexpr std::size_t kIrcv3LegacyUserValueMaximum = 512;

inline bool ValidUserMutationText(std::string_view value, std::size_t maximum) noexcept
{
	return value.size() <= maximum && value.find('\0') == std::string_view::npos;
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

struct Ircv3LegacyMessageAdaptation {
	std::optional<Ircv3AdapterEvent> typed_context;
	std::optional<std::string> legacy_wire;
};

// Preserve the complete parsed message for typed consumers before flattening
// ordinary IRC into the historical command parser. TAGMSG is tag-only protocol
// metadata, including when a server has stripped every tag, and must never be
// presented to the legacy unknown-command/history path.
inline Ircv3LegacyMessageAdaptation AdaptProtocolMessage(
	const comic_chat::ircv3::Message& message)
{
	Ircv3LegacyMessageAdaptation result;
	const bool typed_only = message.command == "TAGMSG";
	if (!message.tags.empty() || typed_only) {
		comic_chat::ircv3::Event context;
		context.type = comic_chat::ircv3::EventType::MessageContext;
		context.source = message.prefix ? *message.prefix : std::string{};
		context.target = message.params.empty() ? std::string{} : message.params.front();
		context.key = message.command;
		result.typed_context = Ircv3AdapterEvent{std::move(context), message};
	}
	if (!typed_only) {
		auto wire = message.SerializeChecked(false);
		if (wire) result.legacy_wire = std::move(*wire);
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
