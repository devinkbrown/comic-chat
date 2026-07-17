#include "../transportuibridge.h"
#include "../ircv3eventbridge.h"

#include <map>
#include <string>

using ::comicchat::net::State;
using comic_chat::modern_ui::TransportState;
using comic_chat::modern_ui::TransportStateFor;

static_assert(TransportStateFor(State::stopped) == TransportState::offline);
static_assert(TransportStateFor(State::resolving) == TransportState::connecting);
static_assert(TransportStateFor(State::connecting) == TransportState::connecting);
static_assert(TransportStateFor(State::proxy_handshake) == TransportState::connecting);
static_assert(TransportStateFor(State::tls_handshake) == TransportState::connecting);
static_assert(TransportStateFor(State::connected) == TransportState::online);
static_assert(TransportStateFor(State::reconnect_wait) == TransportState::reconnecting);

namespace {

using comic_chat::ircv3::Event;
using comic_chat::ircv3::EventType;
using comic_chat::legacy_ui::ClassifyUserMutation;
using comic_chat::legacy_ui::ClassifyStandardReply;
using comic_chat::legacy_ui::ClassifyChannelRename;
using comic_chat::legacy_ui::ConsumeChannelRename;
using comic_chat::legacy_ui::FormatChannelRenameStatus;
using comic_chat::legacy_ui::Ircv3ChannelRenameResult;
using comic_chat::legacy_ui::ConsumeUserMutation;
using comic_chat::legacy_ui::Ircv3StatusSeverity;
using comic_chat::legacy_ui::Ircv3UserMutation;
using comic_chat::legacy_ui::Ircv3UserMutationKind;
using comic_chat::legacy_ui::Ircv3UserMutationResult;

struct FakeUser {
	std::string account;
	std::string identity;
	std::string realname;
	bool away{};
};

int failures{};

void Check(bool condition)
{
	if (!condition) ++failures;
}

void TestTypedUserMutationClassification()
{
	Event account;
	account.type = EventType::Account;
	account.source = "Bob";
	account.value = "bob-account";
	auto mutation = ClassifyUserMutation(account);
	Check(mutation.kind == Ircv3UserMutationKind::account && mutation.nickname == "Bob" &&
		mutation.value == "bob-account" && mutation.active);

	// SASL numerics identify the local user in target rather than source.
	account.source.clear();
	account.target = "LocalNick";
	account.value.clear();
	mutation = ClassifyUserMutation(account);
	Check(mutation.kind == Ircv3UserMutationKind::account && mutation.nickname == "LocalNick" &&
		!mutation.active);

	Event away;
	away.type = EventType::Away;
	away.source = "Bob";
	away.value = "lunch";
	mutation = ClassifyUserMutation(away);
	Check(mutation.kind == Ircv3UserMutationKind::away && mutation.active &&
		mutation.value == "lunch");
	away.value.clear();
	Check(!ClassifyUserMutation(away).active);

	Event host;
	host.type = EventType::HostChanged;
	host.source = "Bob";
	host.key = "new-user";
	host.value = "cloak.example";
	mutation = ClassifyUserMutation(host);
	Check(mutation.kind == Ircv3UserMutationKind::host && mutation.value == "new-user" &&
		mutation.secondary == "cloak.example");

	Event realname;
	realname.type = EventType::RealnameChanged;
	realname.source = "Bob";
	realname.value = "Bob Example";
	mutation = ClassifyUserMutation(realname);
	Check(mutation.kind == Ircv3UserMutationKind::realname && mutation.value == "Bob Example");

	Event unrelated;
	unrelated.type = EventType::Typing;
	unrelated.source = "Bob";
	Check(ClassifyUserMutation(unrelated).kind == Ircv3UserMutationKind::none);

	host.key.clear();
	Check(ClassifyUserMutation(host).kind == Ircv3UserMutationKind::none);
	host.key = "new@user";
	Check(ClassifyUserMutation(host).kind == Ircv3UserMutationKind::none);
	realname.source.clear();
	realname.target.clear();
	Check(ClassifyUserMutation(realname).kind == Ircv3UserMutationKind::none);

	account.source = std::string("Bob\0spoof", 9);
	account.target.clear();
	Check(ClassifyUserMutation(account).kind == Ircv3UserMutationKind::none);
	account.source = "@Bob";
	Check(ClassifyUserMutation(account).kind == Ircv3UserMutationKind::none);
	realname.source = "Bob";
	realname.value = std::string("unsafe\0name", 11);
	Check(ClassifyUserMutation(realname).kind == Ircv3UserMutationKind::none);
}

void TestTypedUserMutationsReachOwnedModels()
{
	std::map<std::string, FakeUser, std::less<>> users;
	users.emplace("Bob", FakeUser{"old-account", "old@host", "Old Name", false});
	users.emplace("LocalNick", FakeUser{});
	const auto find = [&users](std::string_view nickname) -> FakeUser* {
		const auto found = users.find(nickname);
		return found == users.end() ? nullptr : &found->second;
	};
	const auto apply = [](FakeUser& user, const Ircv3UserMutation& mutation) {
		switch (mutation.kind) {
		case Ircv3UserMutationKind::account:
			user.account = mutation.value;
			break;
		case Ircv3UserMutationKind::away:
			user.away = mutation.active;
			break;
		case Ircv3UserMutationKind::host:
			user.identity = std::string(mutation.value) + '@' + std::string(mutation.secondary);
			break;
		case Ircv3UserMutationKind::realname:
			user.realname = mutation.value;
			break;
		case Ircv3UserMutationKind::none:
			break;
		}
	};

	Event event;
	event.type = EventType::Account;
	event.source = "Bob";
	event.value = "new-account";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);
	Check(users.at("Bob").account == "new-account" && users.at("Bob").identity == "old@host" &&
		users.at("Bob").realname == "Old Name");
	event.value.clear();
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);
	Check(users.at("Bob").account.empty());
	event.value = "new-account";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);

	event.type = EventType::HostChanged;
	event.key = "new-user";
	event.value = "cloak.example";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);
	Check(users.at("Bob").identity == "new-user@cloak.example" &&
		users.at("Bob").account == "new-account" && users.at("Bob").realname == "Old Name");

	event.type = EventType::RealnameChanged;
	event.key.clear();
	event.value = "Bob Example";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);
	Check(users.at("Bob").realname == "Bob Example" &&
		users.at("Bob").identity == "new-user@cloak.example");

	event.type = EventType::Away;
	event.value = "gone";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);
	Check(users.at("Bob").away);
	event.value.clear();
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);
	Check(!users.at("Bob").away);

	event.type = EventType::Account;
	event.source.clear();
	event.target = "LocalNick";
	event.value = "local-account";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);
	Check(users.at("LocalNick").account == "local-account");

	event.source = "Missing";
	event.target.clear();
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::unknown_user);
	Check(users.size() == 2);

	event.type = EventType::Typing;
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::ignored);
}

void TestStandardReplyPresentation()
{
	Event reply;
	reply.type = EventType::StandardReply;
	reply.key = "FAIL";
	reply.target = "CHATHISTORY";
	reply.value = "INVALID_PARAMS";
	reply.detail = "The selector is invalid";
	auto presentation = ClassifyStandardReply(reply);
	Check(presentation && presentation->severity == Ircv3StatusSeverity::error &&
		presentation->text == "[FAIL] CHATHISTORY/INVALID_PARAMS: The selector is invalid");

	reply.key = "WARN";
	presentation = ClassifyStandardReply(reply);
	Check(presentation && presentation->severity == Ircv3StatusSeverity::warning);
	reply.key = "NOTE";
	presentation = ClassifyStandardReply(reply);
	Check(presentation && presentation->severity == Ircv3StatusSeverity::information);

	reply.key = "NOTICE";
	Check(!ClassifyStandardReply(reply));
	reply.key = "FAIL";
	reply.target = "BAD COMMAND";
	Check(!ClassifyStandardReply(reply));
	reply.target = "PRIVMSG";
	reply.value = std::string("BAD\0CODE", 8);
	Check(!ClassifyStandardReply(reply));
	reply.value = "REJECTED";
	reply.detail = std::string(513, 'x');
	Check(!ClassifyStandardReply(reply));
	reply.detail = std::string("format\001control", 14);
	presentation = ClassifyStandardReply(reply);
	Check(presentation && presentation->text == "[FAIL] PRIVMSG/REJECTED: format control");
	reply.detail = std::string("unsafe\0description", 18);
	Check(!ClassifyStandardReply(reply));
	reply.type = EventType::ProtocolError;
	reply.detail = "not a server reply";
	Check(!ClassifyStandardReply(reply));
}

void TestChannelRenameConsumer()
{
	Event event;
	event.type = EventType::ChannelRenamed;
	event.target = "#OldRoom";
	event.value = "#NewRoom";
	event.detail = "Clearer name";
	auto rename = ClassifyChannelRename(event);
	Check(rename && rename->previous == "#OldRoom" && rename->current == "#NewRoom" &&
		FormatChannelRenameStatus(*rename) ==
			"[RENAME] #OldRoom -> #NewRoom: Clearer name");

	std::string channel = "#OldRoom";
	const auto matches = [&channel](std::string_view candidate) {
		return candidate == channel;
	};
	const auto apply = [&channel](const auto& change) {
		channel = change.current;
	};
	Check(ConsumeChannelRename(event, matches, apply) == Ircv3ChannelRenameResult::applied &&
		channel == "#NewRoom");
	channel = "#Different";
	Check(ConsumeChannelRename(event, matches, apply) == Ircv3ChannelRenameResult::not_target &&
		channel == "#Different");

	event.detail = std::string("control\x01reason", 14);
	rename = ClassifyChannelRename(event);
	Check(rename && FormatChannelRenameStatus(*rename) ==
		"[RENAME] #OldRoom -> #NewRoom: control reason");
	for (const auto& invalid : {
		std::string{}, std::string("#bad room"), std::string("#bad,room"),
		std::string("#bad\0room", 9), std::string(256, 'x')}) {
		event.value = invalid;
		Check(!ClassifyChannelRename(event));
	}
	event.value = "#NewRoom";
	event.detail = std::string(513, 'x');
	Check(!ClassifyChannelRename(event));
	event.type = EventType::Typing;
	event.detail.clear();
	Check(!ClassifyChannelRename(event));
}

} // namespace

int main()
{
	TestTypedUserMutationClassification();
	TestTypedUserMutationsReachOwnedModels();
	TestStandardReplyPresentation();
	TestChannelRenameConsumer();
	return failures == 0 ? 0 : 1;
}
