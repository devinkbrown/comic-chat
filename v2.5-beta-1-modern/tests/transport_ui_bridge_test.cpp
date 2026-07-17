#include "../transportuibridge.h"
#include "../ircv3eventbridge.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

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
using comic_chat::legacy_ui::ClassifyReactionStatus;
using comic_chat::legacy_ui::IsWhitelistedMetadataKey;
using comic_chat::legacy_ui::ClassifyChannelRename;
using comic_chat::legacy_ui::ConsumeChannelRename;
using comic_chat::legacy_ui::FormatChannelRenameStatus;
using comic_chat::legacy_ui::Ircv3ChannelRenameResult;
using comic_chat::legacy_ui::ConsumeUserMutation;
using comic_chat::legacy_ui::Ircv3StatusSeverity;
using comic_chat::legacy_ui::Ircv3UserMutation;
using comic_chat::legacy_ui::Ircv3UserMutationKind;
using comic_chat::legacy_ui::Ircv3UserMutationResult;
using comic_chat::legacy_ui::AdaptProtocolMessage;
using comic_chat::legacy_ui::HasSafeLegacyDispatchShape;
using comic_chat::legacy_ui::PrepareLegacyProtocolWire;
using comic_chat::legacy_ui::PrepareExplicitNamesRequest;
using comic_chat::legacy_ui::IrcProtocolLineBudget;
using comic_chat::legacy_ui::IrcTransportIngressAction;
using comic_chat::legacy_ui::IrcTransportIngressGate;
using comic_chat::legacy_ui::IrcTransportIngressPhase;
using comic_chat::legacy_ui::NormalizeLegacyNamesReply;
using comic_chat::legacy_ui::ClassifyNamesIdentities;
using comic_chat::legacy_ui::ConsumeNamesIdentities;

struct FakeUser {
	std::string account;
	std::string identity;
	std::string realname;
	bool away{};
	// New model fields exercised by items 1.4/1.6. Appended after the original
	// four so the existing positional/designated inits stay valid.
	std::string profile_url;
	std::string status;
	// UF_TYPING-style member-list state. In comic view the real client must drive
	// this through the STATE overlay ladder, not the flag ladder (memblst.cpp
	// bypasses flags there); the bridge only reports the boolean.
	bool typing{};
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
	users.emplace("Bob", FakeUser{"old-account", "old@host", "Old Name", false, {}, {}, false});
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
		case Ircv3UserMutationKind::typing:
			user.typing = mutation.active;
			break;
		case Ircv3UserMutationKind::metadata:
			// mutation.secondary names the whitelisted key; route it to the owned
			// field exactly as SetAccount/SetRealName would.
			if (mutation.secondary == "url")
				user.profile_url = mutation.value;
			else if (mutation.secondary == "status")
				user.status = mutation.value;
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

void TestLegacyDispatchShapeGate()
{
	comic_chat::ircv3::Message message;
	message.prefix = "nick!user@host";
	for (const std::string_view command : {"JOIN", "PART", "KILL", "001"}) {
		message.command = std::string(command);
		message.params.clear();
		Check(!HasSafeLegacyDispatchShape(message));
		auto rejected = AdaptProtocolMessage(message);
		Check(rejected.rejected_legacy_shape && !rejected.legacy_wire);
		message.params = {command == "001" ? "localnick" : "#ink"};
		Check(HasSafeLegacyDispatchShape(message));
		Check(AdaptProtocolMessage(message).legacy_wire.has_value());
	}

	message.command = "WHISPER";
	for (const auto& malformed : {
		std::vector<std::string>{},
		std::vector<std::string>{"#ink"},
		std::vector<std::string>{"#ink", "nick"}}) {
		message.params = malformed;
		Check(!HasSafeLegacyDispatchShape(message));
		Check(AdaptProtocolMessage(message).rejected_legacy_shape);
	}
	message.params = {"#ink", "nick", "hello"};
	Check(HasSafeLegacyDispatchShape(message));
	Check(AdaptProtocolMessage(message).legacy_wire.has_value());

	message.command = "NOTICE";
	message.params.clear();
	Check(HasSafeLegacyDispatchShape(message));
	Check(AdaptProtocolMessage(message).legacy_wire.has_value());

	for (const auto& [command, params, suffix] : {
		std::tuple{"NICK", std::vector<std::string>{"newnick"}, std::string{"NICK :newnick\r\n"}},
		std::tuple{"JOIN", std::vector<std::string>{"#ink"}, std::string{"JOIN :#ink\r\n"}},
		std::tuple{"INVITE", std::vector<std::string>{"me", "#ink"}, std::string{"INVITE me :#ink\r\n"}},
		std::tuple{"PRIVMSG", std::vector<std::string>{"#ink", "hello"}, std::string{"PRIVMSG #ink :hello\r\n"}},
		std::tuple{"NOTICE", std::vector<std::string>{"me", "hello"}, std::string{"NOTICE me :hello\r\n"}},
		std::tuple{"KICK", std::vector<std::string>{"#ink", "nick", "reason"}, std::string{"KICK #ink nick :reason\r\n"}},
		std::tuple{"WHISPER", std::vector<std::string>{"#ink", "nick", "hello"}, std::string{"WHISPER #ink nick :hello\r\n"}},
		std::tuple{"001", std::vector<std::string>{"me", "welcome"}, std::string{"001 me :welcome\r\n"}}}) {
		message.command = command;
		message.params = params;
		auto wire = PrepareLegacyProtocolWire(message);
		Check(wire && wire->ends_with(suffix));
	}
}

void TestExtendedJoinPreservesTypedIdentityAndLegacyChannel()
{
	const auto parsed = comic_chat::ircv3::Message::Parse(
		":Tiki!user@example.test JOIN #ink tiki-account :Tiki Example\r\n");
	Check(parsed.has_value());
	if (!parsed) return;

	const auto adapted = AdaptProtocolMessage(*parsed);
	Check(!adapted.rejected_legacy_shape);
	Check(adapted.typed_context.has_value() && adapted.typed_context->message.has_value());
	if (adapted.typed_context && adapted.typed_context->message) {
		const auto& complete = *adapted.typed_context->message;
		Check(complete.params ==
			std::vector<std::string>{"#ink", "tiki-account", "Tiki Example"});
	}
	Check(adapted.legacy_wire ==
		std::optional<std::string>{":Tiki!user@example.test JOIN :#ink\r\n"});

	comic_chat::ircv3::Message malformed = *parsed;
	malformed.params.pop_back();
	const auto rejected = AdaptProtocolMessage(malformed);
	Check(rejected.rejected_legacy_shape && !rejected.legacy_wire);
}

void TestNamesExtensionsNormalizeBeforeLegacyMembership()
{
	const auto parsed = comic_chat::ircv3::Message::Parse(
		":server 353 me = #ink :~&@%+Owner!owner@example.test @+Op!op@example.test "
		"+Voice!voice@example.test Plain!plain@example.test\r\n");
	Check(parsed.has_value());
	if (!parsed) return;

	const auto normalized = NormalizeLegacyNamesReply(*parsed, "(qaohv)~&@%+");
	Check(normalized.has_value());
	if (normalized)
		Check(normalized->params.back() == ".Owner @Op +Voice Plain");
	const auto wire = PrepareLegacyProtocolWire(*parsed, "(qaohv)~&@%+");
	Check(wire == std::optional<std::string>{
		":server 353 me = #ink :.Owner @Op +Voice Plain\r\n"});

	comic_chat::ircv3::Message single = *parsed;
	single.params.back() = "@Only!user@example.test";
	const auto single_wire = PrepareLegacyProtocolWire(single, "(ov)@+");
	Check(single_wire && single_wire->ends_with("353 me = #ink :@Only\r\n"));

	for (const std::string_view invalid : {
		"ov)@+", "(ov@+", "(ov)@", "(oo)@+", "(ov)@@"})
		Check(!NormalizeLegacyNamesReply(*parsed, invalid));

	// An empty PREFIX= is a legitimate ISUPPORT token: the network has no status
	// prefixes. It must normalize with an empty prefix set -- stripping nothing,
	// still splitting hostmasks -- not drop the whole 353 and leave the member
	// list empty.
	const auto no_prefix = comic_chat::ircv3::Message::Parse(
		":server 353 me = #ink :Alice!a@host Bob!b@host\r\n");
	Check(no_prefix.has_value());
	if (no_prefix) {
		std::vector<std::string> no_prefix_identities;
		const auto flat = NormalizeLegacyNamesReply(*no_prefix, "", &no_prefix_identities);
		Check(flat.has_value());
		if (flat)
			Check(flat->params.back() == "Alice Bob");
		Check(no_prefix_identities == std::vector<std::string>{
			"Alice", "a", "host", "Bob", "b", "host"});
	}

	comic_chat::ircv3::Message malformed = *parsed;
	malformed.params.pop_back();
	Check(!HasSafeLegacyDispatchShape(malformed));
	Check(AdaptProtocolMessage(malformed).rejected_legacy_shape);
}

// userhost-in-names makes the server send nick!user@host inside 353. The legacy
// member model must keep receiving bare nicknames, and the hostmask must reach
// CUserInfo::SetFullName through the same typed host mutation chghost uses.
void TestUserhostInNamesSuppliesHostmaskToLegacyModel()
{
	const auto parsed = comic_chat::ircv3::Message::Parse(
		":server 353 me = #ink :@+Op!opuser@op.example.test "
		"Plain!plainuser@plain.example.test NoMask\r\n");
	Check(parsed.has_value());
	if (!parsed) return;

	// Safety property: multi-prefix and the hostmask are both stripped, so the
	// nickname column can never be polluted by user@host text.
	std::vector<std::string> identities;
	const auto normalized = NormalizeLegacyNamesReply(*parsed, "(ov)@+", &identities);
	Check(normalized.has_value());
	if (normalized)
		Check(normalized->params.back() == "@Op Plain NoMask");
	Check(identities == std::vector<std::string>{
		"Op", "opuser", "op.example.test",
		"Plain", "plainuser", "plain.example.test"});

	const auto adapted = AdaptProtocolMessage(*parsed, "(ov)@+");
	Check(!adapted.rejected_legacy_shape);
	Check(adapted.legacy_wire ==
		std::optional<std::string>{":server 353 me = #ink :@Op Plain NoMask\r\n"});
	Check(adapted.typed_context.has_value());
	if (!adapted.typed_context) return;

	const auto mutations = ClassifyNamesIdentities(adapted.typed_context->event);
	Check(mutations.size() == 2);
	if (mutations.size() == 2) {
		Check(mutations[0].kind == Ircv3UserMutationKind::host &&
			mutations[0].nickname == "Op" && mutations[0].value == "opuser" &&
			mutations[0].secondary == "op.example.test" && mutations[0].active);
		Check(mutations[1].kind == Ircv3UserMutationKind::host &&
			mutations[1].nickname == "Plain" && mutations[1].value == "plainuser" &&
			mutations[1].secondary == "plain.example.test");
	}

	// Legacy consumer: identical find/apply pair to the chghost host mutation.
	std::map<std::string, FakeUser, std::less<>> users;
	users.emplace("Op", FakeUser{});
	users.emplace("Plain", FakeUser{});
	users.emplace("NoMask", FakeUser{});
	const auto find = [&users](std::string_view nickname) -> FakeUser* {
		const auto found = users.find(nickname);
		return found == users.end() ? nullptr : &found->second;
	};
	const auto apply = [](FakeUser& user, const Ircv3UserMutation& mutation) {
		if (mutation.kind == Ircv3UserMutationKind::host)
			user.identity = std::string(mutation.value) + '@' + std::string(mutation.secondary);
	};
	auto result = ConsumeNamesIdentities(adapted.typed_context->event, find, apply);
	Check(!result.ignored && result.applied == 2 && result.unknown_user == 0);
	Check(users.at("Op").identity == "opuser@op.example.test");
	Check(users.at("Plain").identity == "plainuser@plain.example.test");
	// A token without a hostmask still joins the room; it just gains no identity.
	Check(users.at("NoMask").identity.empty());

	// Rooms that do not hold the user must report it rather than invent members.
	users.erase("Plain");
	result = ConsumeNamesIdentities(adapted.typed_context->event, find, apply);
	Check(result.applied == 1 && result.unknown_user == 1 && users.size() == 2);

	// chatview.cpp hands one find/apply pair to both consumers, so a NAMES
	// identity and a later chghost stay interchangeable. The MFC view itself
	// builds only under MSVC; keep that shape compiling here.
	Event chghost;
	chghost.type = EventType::HostChanged;
	chghost.source = "Op";
	chghost.key = "opuser";
	chghost.value = "cloak.example.test";
	Check(ConsumeUserMutation(chghost, find, apply) == Ircv3UserMutationResult::applied);
	Check(users.at("Op").identity == "opuser@cloak.example.test");

	// A 353 with no hostmask at all keeps working and emits no typed context.
	comic_chat::ircv3::Message bare = *parsed;
	bare.params.back() = "@Op Plain";
	std::vector<std::string> none;
	const auto bare_normalized = NormalizeLegacyNamesReply(bare, "(ov)@+", &none);
	Check(bare_normalized && bare_normalized->params.back() == "@Op Plain" && none.empty());
	const auto bare_adapted = AdaptProtocolMessage(bare, "(ov)@+");
	Check(bare_adapted.legacy_wire ==
		std::optional<std::string>{":server 353 me = #ink :@Op Plain\r\n"});
	Check(!bare_adapted.typed_context.has_value());

	// A '!' without a complete user@host keeps the nickname and drops the identity.
	comic_chat::ircv3::Message half = *parsed;
	half.params.back() = "Half!useronly Empty!";
	std::vector<std::string> half_out;
	const auto half_normalized = NormalizeLegacyNamesReply(half, "(ov)@+", &half_out);
	Check(half_normalized && half_normalized->params.back() == "Half Empty" && half_out.empty());

	// Oversize identity parts are dropped, never truncated into the model, and
	// never cost the member their place in the room.
	comic_chat::ircv3::Message oversize_host = *parsed;
	oversize_host.params.back() = "Big!user@" + std::string(256, 'h');
	std::vector<std::string> big_out;
	const auto big_normalized = NormalizeLegacyNamesReply(oversize_host, "(ov)@+", &big_out);
	Check(big_normalized && big_normalized->params.back() == "Big" && big_out.empty());

	// Fail-closed: malformed input yields nullopt and never partially writes out.
	// An empty PREFIX is not malformed (it means "no status prefixes") and is
	// covered as a positive case in TestNamesExtensionsNormalizeBeforeLegacyMembership.
	for (const std::string_view invalid : {
		"ov)@+", "(ov@+", "(ov)@", "(oo)@+", "(ov)@@"}) {
		std::vector<std::string> untouched{"stale"};
		Check(!NormalizeLegacyNamesReply(*parsed, invalid, &untouched));
		Check(untouched == std::vector<std::string>{"stale"});
	}
	// A good token ahead of a rejected one must not leave its identity behind:
	// the reply commits every identity or none.
	comic_chat::ircv3::Message oversize_nick = *parsed;
	oversize_nick.params.back() = "Good!gooduser@good.example.test " +
		std::string(256, 'n') + "!user@host.example.test";
	std::vector<std::string> oversize_out{"stale"};
	Check(!NormalizeLegacyNamesReply(oversize_nick, "(ov)@+", &oversize_out));
	Check(oversize_out == std::vector<std::string>{"stale"});
	Check(!AdaptProtocolMessage(oversize_nick, "(ov)@+").legacy_wire);
	Check(!AdaptProtocolMessage(oversize_nick, "(ov)@+").typed_context.has_value());

	// A corrupt or foreign event must not reach the model.
	Event corrupt;
	corrupt.type = EventType::MessageContext;
	corrupt.key = "353";
	corrupt.context = {"Op", "opuser"};
	Check(ClassifyNamesIdentities(corrupt).empty());
	corrupt.context = {"Op", "op@user", "op.example.test"};
	Check(ClassifyNamesIdentities(corrupt).empty());
	corrupt.context = {"@Op", "opuser", "op.example.test"};
	Check(ClassifyNamesIdentities(corrupt).empty());
	corrupt.context = {"Op", "opuser", std::string("host\0spoof", 10)};
	Check(ClassifyNamesIdentities(corrupt).empty());
	corrupt.context = {"Op", "opuser", "op.example.test"};
	Check(ClassifyNamesIdentities(corrupt).size() == 1);
	corrupt.key = "352";
	Check(ClassifyNamesIdentities(corrupt).empty());
	corrupt.key = "353";
	corrupt.type = EventType::Typing;
	Check(ClassifyNamesIdentities(corrupt).empty());
	Check(ConsumeNamesIdentities(corrupt, find, apply).ignored);
}

void TestNoImplicitNamesBuildsExplicitBoundedQuery()
{
	Check(!PrepareExplicitNamesRequest(false, "#ink"));
	Check(PrepareExplicitNamesRequest(true, "#ink") ==
		std::optional<std::string>{"NAMES #ink\r\n"});
	for (const auto& invalid : {
		std::string{}, std::string("#bad room"), std::string("#bad,room"),
		std::string("#bad\0room", 9), std::string(256, 'x')})
		Check(!PrepareExplicitNamesRequest(true, invalid));
}

void TestTransportIngressPhaseAndWorkGates()
{
	IrcTransportIngressGate gate;
	gate.Begin(7);
	const auto wire = std::make_shared<const std::vector<std::byte>>(
		std::vector<std::byte>{std::byte{'P'}});
	Check(gate.Classify({6, comicchat::net::BytesReceived{wire}}) ==
		IrcTransportIngressAction::stale);
	Check(gate.Classify({7, comicchat::net::BytesReceived{wire}}) ==
		IrcTransportIngressAction::out_of_order);
	Check(gate.Classify({7, comicchat::net::Connected{"irc.example", "127.0.0.1", true, false}}) ==
		IrcTransportIngressAction::accepted);
	Check(gate.phase() == IrcTransportIngressPhase::connected);
	Check(gate.Classify({7, comicchat::net::BytesReceived{wire}}) ==
		IrcTransportIngressAction::accepted);
	Check(gate.Classify({7, comicchat::net::Connected{"irc.example", "127.0.0.1", true, false}}) ==
		IrcTransportIngressAction::out_of_order);
	Check(gate.Classify({7, comicchat::net::Closed{"retry", std::chrono::milliseconds{50}}}) ==
		IrcTransportIngressAction::accepted);
	Check(gate.phase() == IrcTransportIngressPhase::reconnecting);
	Check(gate.Classify({7, comicchat::net::BytesReceived{wire}}) ==
		IrcTransportIngressAction::out_of_order);
	Check(gate.Classify({7, comicchat::net::Connected{"irc.example", "127.0.0.1", true, false}}) ==
		IrcTransportIngressAction::accepted);
	Check(gate.Classify({7, comicchat::net::Closed{"done", std::chrono::milliseconds{0}}}) ==
		IrcTransportIngressAction::accepted);
	Check(gate.phase() == IrcTransportIngressPhase::stopped);
	Check(gate.Classify({7, comicchat::net::PingDue{}}) ==
		IrcTransportIngressAction::out_of_order);
	gate.Stop();
	Check(gate.Classify({7, comicchat::net::Diagnostic{"late", "ignored"}}) ==
		IrcTransportIngressAction::stale);

	IrcProtocolLineBudget budget{512};
	Check(budget.Consume(256) && budget.remaining() == 256);
	Check(!budget.Consume(257) && budget.remaining() == 256);
	Check(budget.Consume(256) && budget.remaining() == 0);
	Check(!budget.Consume(1));
}

// 1.4 -- a draft/typing typed event becomes a per-nick UF_TYPING-style mutation.
void TestTypingTypedEventBecomesMemberState()
{
	Event typing;
	typing.type = EventType::Typing;
	typing.source = "Bob";
	typing.key = "active";
	auto mutation = ClassifyUserMutation(typing);
	Check(mutation.kind == Ircv3UserMutationKind::typing && mutation.nickname == "Bob" &&
		mutation.value == "active" && mutation.active);

	typing.key = "paused";
	mutation = ClassifyUserMutation(typing);
	Check(mutation.kind == Ircv3UserMutationKind::typing && !mutation.active);
	typing.key = "done";
	mutation = ClassifyUserMutation(typing);
	Check(mutation.kind == Ircv3UserMutationKind::typing && !mutation.active);

	// A Typing event with no/garbage state token is malformed and yields none, so
	// the historical "unrelated Typing event" path keeps classifying to none.
	typing.key.clear();
	Check(ClassifyUserMutation(typing).kind == Ircv3UserMutationKind::none);
	typing.key = "typing";
	Check(ClassifyUserMutation(typing).kind == Ircv3UserMutationKind::none);
	// A prefixed or null-poisoned source is rejected like every other mutation.
	typing.key = "active";
	typing.source = "@Bob";
	Check(ClassifyUserMutation(typing).kind == Ircv3UserMutationKind::none);
	typing.source = std::string("Bob\0spoof", 9);
	Check(ClassifyUserMutation(typing).kind == Ircv3UserMutationKind::none);

	// Reaches the owned model through the same find/apply pair as chghost.
	std::map<std::string, FakeUser, std::less<>> users;
	users.emplace("Bob", FakeUser{});
	const auto find = [&users](std::string_view nickname) -> FakeUser* {
		const auto found = users.find(nickname);
		return found == users.end() ? nullptr : &found->second;
	};
	const auto apply = [](FakeUser& user, const Ircv3UserMutation& m) {
		if (m.kind == Ircv3UserMutationKind::typing) user.typing = m.active;
	};
	Event event;
	event.type = EventType::Typing;
	event.source = "Bob";
	event.key = "active";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);
	Check(users.at("Bob").typing);
	event.key = "done";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);
	Check(!users.at("Bob").typing);
	event.source = "Ghost";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::unknown_user);
}

// 1.5 -- a message-scoped reaction cannot be a user mutation; the cheapest honest
// consumer is a bounded status-line presentation on the AddToStatus path.
void TestReactionClassifiesToStatusLine()
{
	Event reaction;
	reaction.type = EventType::Reaction;
	reaction.source = "Bob";
	reaction.target = "#ink";
	reaction.key = "react";
	reaction.value = ":tada:";
	reaction.context = {"msgid=abc123"};
	auto status = ClassifyReactionStatus(reaction);
	Check(status && status->severity == Ircv3StatusSeverity::information &&
		status->text == "[REACT] Bob: :tada: (msgid abc123)");

	reaction.key = "unreact";
	status = ClassifyReactionStatus(reaction);
	Check(status && status->text == "[UNREACT] Bob: :tada: (msgid abc123)");

	// Without a msgid the line still presents, just message-anonymous.
	reaction.key = "react";
	reaction.context.clear();
	status = ClassifyReactionStatus(reaction);
	Check(status && status->text == "[REACT] Bob: :tada:");

	// Other context entries are ignored; only msgid is lifted.
	reaction.context = {"reply=xyz", "bot", "msgid=id42"};
	status = ClassifyReactionStatus(reaction);
	Check(status && status->text == "[REACT] Bob: :tada: (msgid id42)");

	// Control characters in the reaction value are scrubbed, never emitted raw.
	reaction.context.clear();
	reaction.value = std::string("ta\001da", 5);
	status = ClassifyReactionStatus(reaction);
	Check(status && status->text == "[REACT] Bob: ta da");

	// A reaction is never a user mutation and never appears on the mutation path.
	reaction.value = ":tada:";
	Check(ClassifyUserMutation(reaction).kind == Ircv3UserMutationKind::none);

	// Malformed reactions classify to nothing (fail-closed).
	reaction.key = "typing";
	Check(!ClassifyReactionStatus(reaction));
	reaction.key = "react";
	reaction.value.clear();
	Check(!ClassifyReactionStatus(reaction));
	reaction.value = ":tada:";
	reaction.source = "@Bob";
	Check(!ClassifyReactionStatus(reaction));
	reaction.source = "Bob";
	reaction.value = std::string(513, 'x');
	Check(!ClassifyReactionStatus(reaction));
	reaction.value = ":tada:";
	reaction.type = EventType::StandardReply;
	Check(!ClassifyReactionStatus(reaction));
}

// 1.6 -- only the whitelisted user-profile key/value SET (one of six METADATA
// producer shapes) is a user mutation; every other shape is ignored.
void TestMetadataWhitelistBecomesOwnedField()
{
	// The SET names its subject in target, not source, exactly like a SASL numeric.
	Event set;
	set.type = EventType::Metadata;
	set.target = "Bob";
	set.key = "url";
	set.value = "https://bob.example";
	set.context = {"visibility=*"};
	auto mutation = ClassifyUserMutation(set);
	Check(mutation.kind == Ircv3UserMutationKind::metadata && mutation.nickname == "Bob" &&
		mutation.secondary == "url" && mutation.value == "https://bob.example" &&
		mutation.active);

	set.key = "status";
	set.value = "away from keyboard";
	mutation = ClassifyUserMutation(set);
	Check(mutation.kind == Ircv3UserMutationKind::metadata && mutation.secondary == "status" &&
		mutation.value == "away from keyboard");

	// Clearing a whitelisted key (empty value) still classifies, but as inactive.
	set.value.clear();
	mutation = ClassifyUserMutation(set);
	Check(mutation.kind == Ircv3UserMutationKind::metadata && !mutation.active);

	Check(IsWhitelistedMetadataKey("url") && IsWhitelistedMetadataKey("status"));
	Check(!IsWhitelistedMetadataKey("avatar") && !IsWhitelistedMetadataKey("sync-later"));

	// The other five shapes are ignored: non-whitelisted keys (subscribed/
	// unsubscribed/subscriptions/sync-later summaries plus arbitrary profile keys)
	// never classify as a set.
	set.value = "https://bob.example";
	for (const std::string_view ignored_key : {
		"subscribed", "unsubscribed", "subscriptions", "sync-later",
		"metadata", "metadata-subs", "avatar", "display-name"}) {
		set.key = ignored_key;
		Check(ClassifyUserMutation(set).kind == Ircv3UserMutationKind::none);
	}

	// The 766 delete reuses a real key but carries detail="deleted"; it is not a set.
	set.key = "url";
	set.detail = "deleted";
	set.value.clear();
	Check(ClassifyUserMutation(set).kind == Ircv3UserMutationKind::none);
	set.detail.clear();

	// Oversize values are rejected rather than truncated into the model.
	set.value = std::string(513, 'x');
	Check(ClassifyUserMutation(set).kind == Ircv3UserMutationKind::none);

	// Reaches the owned CUserInfo field through the shared find/apply pair.
	std::map<std::string, FakeUser, std::less<>> users;
	users.emplace("Bob", FakeUser{});
	const auto find = [&users](std::string_view nickname) -> FakeUser* {
		const auto found = users.find(nickname);
		return found == users.end() ? nullptr : &found->second;
	};
	const auto apply = [](FakeUser& user, const Ircv3UserMutation& m) {
		if (m.kind != Ircv3UserMutationKind::metadata) return;
		if (m.secondary == "url") user.profile_url = m.value;
		else if (m.secondary == "status") user.status = m.value;
	};
	Event event;
	event.type = EventType::Metadata;
	event.target = "Bob";
	event.key = "url";
	event.value = "https://bob.example";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);
	Check(users.at("Bob").profile_url == "https://bob.example");
	event.key = "status";
	event.value = "brb";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::applied);
	Check(users.at("Bob").status == "brb" &&
		users.at("Bob").profile_url == "https://bob.example");

	// A set for a user the room does not hold is counted, never created.
	event.target = "Ghost";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::unknown_user);
	// An ignored shape never reaches the model.
	event.target = "Bob";
	event.key = "subscribed";
	Check(ConsumeUserMutation(event, find, apply) == Ircv3UserMutationResult::ignored);
}

} // namespace

int main()
{
	TestTypedUserMutationClassification();
	TestTypedUserMutationsReachOwnedModels();
	TestStandardReplyPresentation();
	TestChannelRenameConsumer();
	TestLegacyDispatchShapeGate();
	TestExtendedJoinPreservesTypedIdentityAndLegacyChannel();
	TestNamesExtensionsNormalizeBeforeLegacyMembership();
	TestUserhostInNamesSuppliesHostmaskToLegacyModel();
	TestNoImplicitNamesBuildsExplicitBoundedQuery();
	TestTransportIngressPhaseAndWorkGates();
	TestTypingTypedEventBecomesMemberState();
	TestReactionClassifiesToStatusLine();
	TestMetadataWhitelistBecomesOwnedField();
	return failures == 0 ? 0 : 1;
}
