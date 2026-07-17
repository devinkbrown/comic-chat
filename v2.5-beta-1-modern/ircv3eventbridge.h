#ifndef COMIC_CHAT_IRCV3_EVENT_BRIDGE_H
#define COMIC_CHAT_IRCV3_EVENT_BRIDGE_H

#include "comicchat/net/ircv3.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

struct Ircv3AdapterEvent {
	comic_chat::ircv3::Event event;
	// MessageContext carries the complete parsed message, including typed and
	// unknown tags. UI consumers must not reconstruct context from tag-stripped
	// legacy wire text.
	std::optional<comic_chat::ircv3::Message> message;
};

namespace comic_chat::legacy_ui {

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
