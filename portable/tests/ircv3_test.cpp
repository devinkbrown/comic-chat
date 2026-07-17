#include <comicchat/net/ircv3.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <sstream>
#include <vector>

#include <mbedtls/base64.h>

using comic_chat::ircv3::Engine;
using comic_chat::ircv3::EventType;
using comic_chat::ircv3::CaseMapping;
using comic_chat::ircv3::LineFramer;
using comic_chat::ircv3::Message;
using comic_chat::ircv3::ProcessResult;
using comic_chat::ircv3::SaslConfig;
using comic_chat::ircv3::ServerProfile;
using comic_chat::ircv3::TypingStatus;

namespace {

int failures = 0;

void Check(bool condition, std::string_view description)
{
	if (condition) return;
	std::cerr << "FAIL: " << description << '\n';
	++failures;
}

std::string Encode(std::string_view value)
{
	std::size_t required = 0;
	const auto* source = reinterpret_cast<const unsigned char*>(value.data());
	(void)mbedtls_base64_encode(nullptr, 0, &required, source, value.size());
	std::string encoded(required, '\0');
	std::size_t written = 0;
	Check(mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded.data()),
		encoded.size(), &written, source, value.size()) == 0, "base64 encode");
	encoded.resize(written);
	return encoded;
}

std::string Decode(std::string_view value)
{
	std::size_t required = 0;
	const auto* source = reinterpret_cast<const unsigned char*>(value.data());
	(void)mbedtls_base64_decode(nullptr, 0, &required, source, value.size());
	std::string decoded(required, '\0');
	std::size_t written = 0;
	Check(mbedtls_base64_decode(reinterpret_cast<unsigned char*>(decoded.data()),
		decoded.size(), &written, source, value.size()) == 0, "base64 decode");
	decoded.resize(written);
	return decoded;
}

std::string AuthenticatePayload(const std::string& command)
{
	const std::string prefix = "AUTHENTICATE ";
	Check(command.rfind(prefix, 0) == 0, "AUTHENTICATE command prefix");
	const auto end = command.find("\r\n");
	return command.substr(prefix.size(), end - prefix.size());
}

std::vector<std::string> RequestedNames(const std::vector<std::string>& commands)
{
	std::vector<std::string> names;
	for (const auto& command : commands) {
		if (command.rfind("CAP REQ :", 0) != 0) continue;
		const auto end = command.find("\r\n");
		std::string list = command.substr(9, end - 9);
		std::size_t cursor = 0;
		while (cursor < list.size()) {
			const auto space = list.find(' ', cursor);
			names.push_back(list.substr(cursor, space - cursor));
			cursor = space == std::string::npos ? list.size() : space + 1;
		}
	}
	return names;
}

std::vector<std::string> Enable(Engine* engine, std::string_view capabilities, std::string nick = "Alice")
{
	SaslConfig config;
	config.allow_external = false;
	auto begin = engine->BeginRegistration(config, std::move(nick), true);
	Check(begin == std::vector<std::string>{"CAP LS 302\r\n"}, "CAP 302 starts registration");
	auto ls = engine->Process(":server CAP * LS :" + std::string(capabilities) + "\r\n");
	auto names = RequestedNames(ls.outbound);
	std::vector<std::string> final_outbound;
	for (const auto& name : names) {
		auto ack = engine->Process(":server CAP Alice ACK :" + name + "\r\n");
		final_outbound.insert(final_outbound.end(), ack.outbound.begin(), ack.outbound.end());
	}
	return final_outbound;
}

ProcessResult FeedChallenge(Engine* engine, std::string_view challenge)
{
	const std::string encoded = Encode(challenge);
	ProcessResult result;
	for (std::size_t cursor = 0; cursor < encoded.size(); cursor += 400)
		result = engine->Process("AUTHENTICATE " + encoded.substr(cursor, 400) + "\r\n");
	if (encoded.empty() || encoded.size() % 400 == 0)
		result = engine->Process("AUTHENTICATE +\r\n");
	return result;
}

void StartScram(Engine* engine, std::string nonce = "clientNonce")
{
	SaslConfig config;
	config.authentication_id = "user";
	config.password = "pencil";
	config.allow_external = false;
	config.nonce = std::move(nonce);
	engine->BeginRegistration(config, "Alice", true);
	engine->Process(":server CAP * LS :sasl=SCRAM-SHA-256\r\n");
	engine->Process(":server CAP Alice ACK :sasl\r\n");
	engine->Process("AUTHENTICATE +\r\n");
}

void TestMessageTags()
{
	Message message;
	Check(Message::Parse("@aaa=hello\\sworld;semi=x\\:y;empty=;flag;odd=one\\qtwo;drop=end\\;Case=upper;case=lower;dup=one;dup=two "
		":nick!u@h PRIVMSG #room :hello there\r\n", &message), "parse tagged message");
	Check(message.tags.size() == 9, "all valid final tag keys retained");
	Check(message.FindTag("aaa") && message.FindTag("aaa")->value == "hello world", "space escape");
	Check(message.FindTag("semi") && message.FindTag("semi")->value == "x;y", "semicolon escape");
	Check(message.FindTag("empty") && message.FindTag("empty")->value && message.FindTag("empty")->value->empty(),
		"empty and missing tag value representations are preserved");
	Check(message.FindTag("flag") && !message.FindTag("flag")->value, "valueless tag");
	Check(message.FindTag("odd") && message.FindTag("odd")->value == "oneqtwo", "unknown escape drops slash");
	Check(message.FindTag("drop") && message.FindTag("drop")->value == "end", "trailing slash is dropped");
	Check(message.FindTag("Case") && message.FindTag("Case")->value == "upper" &&
		message.FindTag("case") && message.FindTag("case")->value == "lower", "tag keys are case-sensitive");
	Check(message.FindTag("dup") && message.FindTag("dup")->value == "two", "final duplicate tag wins");
	Check(!Message::Parse("@=odd PRIVMSG #room :invalid\r\n", &message), "empty tag key rejected");
	Check(!Message::Parse("@vendor/ PRIVMSG #room :invalid\r\n", &message), "invalid vendor tag key rejected");
	const auto serialized = message.Serialize();
	Check(serialized.find("aaa=hello\\sworld") != std::string::npos, "tag serialization escapes space");
	Message round_trip;
	Check(Message::Parse(serialized, &round_trip), "serialized message parses");
	Check(round_trip.params == message.params && round_trip.prefix == message.prefix, "message round trip");
}

void TestBoundedFramingAndPong()
{
	LineFramer framer(32);
	std::vector<std::string> lines;
	Check(framer.Push("PING :one\r", &lines) && lines.empty(), "partial IRC line retained");
	Check(framer.Push("\nPING :two\r\n", &lines) && lines.size() == 2, "batched IRC lines framed");
	std::string error;
	Check(!framer.Push(std::string(33, 'x'), &lines, &error) && framer.BufferedBytes() == 0,
		"oversized IRC line rejected and buffer reset");
	Check(!framer.Push(std::string_view("bad\0line", 8), &lines, &error), "NUL-bearing stream rejected");
	const std::array span_input{std::byte{'P'}, std::byte{'I'}, std::byte{'N'}, std::byte{'G'},
		std::byte{' '}, std::byte{':'}, std::byte{'x'}, std::byte{'\r'}, std::byte{'\n'}};
	auto span_lines = framer.Push(span_input);
	Check(span_lines && *span_lines == std::vector<std::string>{"PING :x\r\n"},
		"span framing returns expected lines");

	Engine engine;
	auto pong = engine.Process("PING :opaque token\r\n");
	Check(pong.consumed && pong.outbound == std::vector<std::string>{"PONG :opaque token\r\n"},
		"PING receives an immediate protocol PONG");
}

void TestCapabilityState()
{
	Engine engine;
	SaslConfig config;
	config.allow_external = false;
	engine.BeginRegistration(config, "Alice", false);
	Check(engine.IsEnabled("cap-notify"), "CAP LS 302 implicitly enables cap-notify");
	auto first = engine.Process(":server CAP * LS * :message-tags batch labeled-response echo-message\r\n");
	Check(first.outbound.empty(), "multiline CAP LS waits for final line");
	auto second = engine.Process(":server CAP * LS :account-tag draft/multiline=\"max-bytes=4096,max-lines=20\" "
		"standard-replies sts=port=6697\r\n");
	const auto requested = RequestedNames(second.outbound);
	Check(std::find(requested.begin(), requested.end(), "message-tags") != requested.end(), "message-tags requested");
	Check(std::find(requested.begin(), requested.end(), "labeled-response") != requested.end(), "dependency chain requested");
	Check(std::find(requested.begin(), requested.end(), "draft/multiline") != requested.end(), "multiline requested");
	Check(std::find(requested.begin(), requested.end(), "sts") == requested.end(), "STS observed but never requested");
	Check(engine.CurrentStsPolicy() && engine.CurrentStsPolicy()->port == 6697, "insecure STS upgrade port retained");
	Check(!engine.CurrentStsPolicy()->duration && !engine.CurrentStsPolicy()->preload,
		"insecure connection cannot establish STS persistence");
	Check(!requested.empty(), "supported capabilities are requested");
	engine.Process(":server CAP Alice ACK :" + requested.front() + "\r\n");
	Check(!engine.IsEnabled(requested.front()), "partial ACK does not partially apply an atomic CAP REQ");
	for (std::size_t index = 1; index < requested.size(); ++index)
		engine.Process(":server CAP Alice ACK :" + requested[index] + "\r\n");
	Check(engine.RegistrationFinished(), "CAP END follows complete ACK set");
	Check(engine.IsEnabled("account-tag"), "ACK enables capability");

	auto added = engine.Process(":server CAP Alice NEW :draft/read-marker\r\n");
	Check(RequestedNames(added.outbound) == std::vector<std::string>{"draft/read-marker"}, "CAP NEW requests supported addition");
	engine.Process(":server CAP Alice ACK :draft/read-marker\r\n");
	Check(engine.IsEnabled("draft/read-marker"), "dynamic ACK applies");
	engine.Process(":server CAP Alice DEL :batch\r\n");
	Check(!engine.IsEnabled("batch") && !engine.IsEnabled("labeled-response") &&
		!engine.IsEnabled("draft/multiline"), "CAP DEL removes dependency closure");
	engine.Process(":server CAP Alice DEL :sts cap-notify\r\n");
	Check(engine.CurrentStsPolicy() && engine.IsEnabled("cap-notify"),
		"CAP DEL cannot clear STS policy or implicit cap-notify");
	Engine secure_sts;
	secure_sts.BeginRegistration(config, "Alice", true);
	secure_sts.Process(":server CAP * LS :sts=port=7000,duration=3600,preload\r\n");
	Check(secure_sts.CurrentStsPolicy() && !secure_sts.CurrentStsPolicy()->port &&
		secure_sts.CurrentStsPolicy()->duration == 3600 && secure_sts.CurrentStsPolicy()->preload,
		"TLS-verified STS stores only persistence policy");
	secure_sts.Process(":server CAP Alice NEW :sts=duration=0\r\n");
	Check(!secure_sts.CurrentStsPolicy(), "secure duration zero clears STS persistence");

	Engine expanded;
	expanded.BeginRegistration(config, "Alice", true);
	auto expanded_ls = expanded.Process(":server CAP * LS :message-tags standard-replies monitor away-notify "
		"account-notify chghost setname draft/account-registration draft/extended-isupport draft/oper-tag "
		"draft/pre-away extended-monitor bot\r\n");
	const auto expanded_requested = RequestedNames(expanded_ls.outbound);
	for (const auto* name : {"draft/account-registration", "draft/extended-isupport", "draft/oper-tag",
		"draft/pre-away", "extended-monitor"})
		Check(std::find(expanded_requested.begin(), expanded_requested.end(), name) != expanded_requested.end(),
			"published client capability requested");
	Check(std::find(expanded_requested.begin(), expanded_requested.end(), "monitor") == expanded_requested.end(),
		"MONITOR is discovered through ISUPPORT and never requested as a capability");
	Engine case_sensitive;
	case_sensitive.BeginRegistration(config, "Alice", true);
	auto uppercase = case_sensitive.Process(":server CAP * LS :SASL MESSAGE-TAGS\r\n");
	Check(RequestedNames(uppercase.outbound).empty() && !case_sensitive.IsOffered("sasl"),
		"capability identifiers are case-sensitive opaque names");
}

void TestCapabilityRegistryAndDependencies()
{
	SaslConfig config;
	config.allow_external = false;

	Engine non_caps;
	non_caps.BeginRegistration(config, "Alice", true);
	auto non_cap_ls = non_caps.Process(":server CAP * LS :draft/channel-context draft/react "
		"draft/reply draft/typing draft/netjoin draft/netsplit utf8-only\r\n");
	Check(RequestedNames(non_cap_ls.outbound).empty() && non_caps.RegistrationFinished(),
		"client tags, batch types and UTF8ONLY are never requested as capabilities");

	Engine independent;
	independent.BeginRegistration(config, "Alice", true);
	auto independent_ls = independent.Process(":server CAP * LS :account-tag extended-join "
		"extended-monitor draft/account-registration draft/chathistory draft/oper-tag\r\n");
	const auto independent_names = RequestedNames(independent_ls.outbound);
	for (const auto* name : {"account-tag", "extended-join", "extended-monitor",
		"draft/account-registration", "draft/chathistory", "draft/oper-tag"})
		Check(std::find(independent_names.begin(), independent_names.end(), name) != independent_names.end(),
			"independently negotiable capability is not overconstrained");

	Engine missing_dependencies;
	missing_dependencies.BeginRegistration(config, "Alice", true);
	auto missing_ls = missing_dependencies.Process(":server CAP * LS :labeled-response "
		"draft/metadata-2 draft/message-redaction draft/multiline draft/event-playback\r\n");
	Check(RequestedNames(missing_ls.outbound).empty(),
		"capabilities with normative prerequisites wait for those prerequisites");

	Engine dependencies;
	dependencies.BeginRegistration(config, "Alice", true);
	auto dependency_ls = dependencies.Process(":server CAP * LS :batch message-tags draft/chathistory "
		"labeled-response draft/metadata-2 draft/message-redaction draft/multiline draft/event-playback\r\n");
	const auto dependency_names = RequestedNames(dependency_ls.outbound);
	for (const auto* name : {"labeled-response", "draft/metadata-2", "draft/message-redaction",
		"draft/multiline", "draft/event-playback"})
		Check(std::find(dependency_names.begin(), dependency_names.end(), name) != dependency_names.end(),
			"normative capability prerequisite unlocks its dependent");

	Engine orochi;
	orochi.BeginRegistration(config, "Alice", true);
	auto orochi_ls = orochi.Process(":orochi CAP * LS :message-tags bot\r\n");
	const auto orochi_names = RequestedNames(orochi_ls.outbound);
	Check(std::find(orochi_names.begin(), orochi_names.end(), "bot") != orochi_names.end(),
		"Orochi offered-only bot capability exception is retained");
}

void TestOutboundValidationAndTagPreservation()
{
	Engine engine;
	Check(!engine.PrepareOutgoingChecked("PRIVMSG #room :safe\r\nOPER bad injected\r\n"),
		"embedded line break cannot inject an outbound IRC command");
	Check(engine.PrepareOutgoing("PRIVMSG #room :safe\r\nOPER bad injected\r\n").empty(),
		"compatibility outbound path safely drops malformed lines");
	auto incoming = engine.Process("@time=2026-01-01T00:00:00.000Z;account=bob :Bob PRIVMSG #room :hello\r\n");
	Check(incoming.messages.size() == 1 && incoming.messages[0].FindTag("time") &&
		incoming.messages[0].FindTag("account"), "typed messages preserve metadata for portable presentation");
}

void TestBatches()
{
	Engine engine;
	engine.Process(":server BATCH +m draft/multiline #room\r\n");
	Check(engine.Process("@batch=m :Bob!u@h PRIVMSG #room :hello \r\n").consumed,
		"multiline child is deferred");
	engine.Process("@batch=m;draft/multiline-concat :Bob!u@h PRIVMSG #room :world\r\n");
	engine.Process("@batch=m :Bob!u@h PRIVMSG #room :again\r\n");
	auto multiline = engine.Process(":server BATCH -m\r\n");
	Check(multiline.messages.size() == 1 && multiline.messages[0].params.back() == "hello world\nagain",
		"multiline concatenation and newline rules");
	Check(multiline.events.size() == 1 && multiline.events[0].type == EventType::Multiline,
		"multiline typed event");

	engine.Process(":server BATCH +h chathistory #room\r\n");
	engine.Process("@batch=h;time=2026-01-01T00:00:00.000Z :Bob PRIVMSG #room :old\r\n");
	auto history = engine.Process(":server BATCH -h\r\n");
	Check(history.messages.size() == 1 && history.events.size() == 1 &&
		history.events[0].type == EventType::ChatHistory, "chathistory batch replays with typed event");

	engine.Process(":server BATCH +outer chathistory #room\r\n");
	engine.Process("@batch=outer :server BATCH +inner draft/multiline #room\r\n");
	engine.Process("@batch=inner :Bob PRIVMSG #room :nested \r\n");
	engine.Process("@batch=inner;draft/multiline-concat :Bob PRIVMSG #room :message\r\n");
	auto inner = engine.Process("@batch=outer :server BATCH -inner\r\n");
	Check(inner.messages.empty(), "nested batch completion remains inside its parent");
	auto outer = engine.Process(":server BATCH -outer\r\n");
	Check(outer.messages.size() == 1 && outer.messages[0].params.back() == "nested message",
		"nested multiline is delivered with outer chathistory");

	engine.Process(":server BATCH +bounded chathistory #room\r\n");
	for (std::size_t index = 0; index < 513; ++index)
		engine.Process("@batch=bounded :Bob JOIN #room\r\n");
	auto bounded = engine.Process(":server BATCH -bounded\r\n");
	Check(bounded.messages.empty(), "oversized batch state is dropped within deterministic bounds");
}

void TestStateEvents()
{
	Engine engine;
	const std::vector<std::pair<std::string, EventType>> cases = {
		{":Bob!u@h ACCOUNT bob-account\r\n", EventType::Account},
		{":Bob!u@h AWAY :gone\r\n", EventType::Away},
		{":Bob!u@h CHGHOST user cloak.example\r\n", EventType::HostChanged},
		{":Bob!u@h SETNAME :Bob Example\r\n", EventType::RealnameChanged},
		{":server RENAME #old #new :moving\r\n", EventType::ChannelRenamed},
		{":server MARKREAD #new timestamp=2026-01-01T00:00:00.000Z\r\n", EventType::ReadMarker},
		{":server METADATA Bob key * :value\r\n", EventType::Metadata},
		{":Bob!u@h REDACT #new abc123 :mistake\r\n", EventType::Redaction},
		{":server FAIL CHATHISTORY INVALID_PARAMS :bad selector\r\n", EventType::StandardReply},
	};
	for (const auto& [wire, type] : cases) {
		auto result = engine.Process(wire);
		Check(result.consumed && result.events.size() == 1 && result.events[0].type == type,
			"typed state event");
	}
	Check(engine.Accounts().at("bob") == "bob-account", "account state retained");
	Check(engine.AwayMessages().at("bob") == "gone", "away state retained");
	Check(engine.Hosts().at("bob") == "user@cloak.example", "host state retained");
	Check(engine.Realnames().at("bob") == "Bob Example", "realname state retained");
	Check(engine.ReadMarkers().at("#new").find("timestamp=") == 0, "read marker retained");
	Check(engine.Metadata().at("bob").at("key") == "value", "metadata retained");
	Check(engine.RedactedMessageIds().count("abc123") == 1, "redaction retained");
	auto logged_in = engine.Process(":server 900 Bob Bob!u@h bob-account :You are now logged in\r\n");
	Check(logged_in.events.size() == 1 && logged_in.events[0].type == EventType::Account &&
		engine.Accounts().at("bob") == "bob-account", "SASL login numeric updates typed account state");
	engine.Process(":server 901 Bob Bob!u@h :You are now logged out\r\n");
	Check(!engine.Accounts().contains("bob"), "SASL logout numeric clears account state");
}

void TestLabelsAndEchoes()
{
	Engine engine;
	Enable(&engine, "message-tags batch labeled-response echo-message");
	const auto outgoing = engine.PrepareOutgoing("PRIVMSG #room :hello\r\n");
	Message sent;
	Check(Message::Parse(outgoing, &sent), "prepared outgoing parses");
	const auto* label = sent.FindTag("label");
	Check(label && label->value, "labeled-response adds opaque label");
	const auto echoed = engine.Process("@label=" + *label->value + " :Alice!u@h PRIVMSG #room :hello\r\n");
	Check(echoed.consumed && echoed.messages.empty(), "echo-message does not create duplicate balloon");
	Check(echoed.events.size() == 1 && echoed.events[0].type == EventType::LabeledResponse,
		"echo still resolves label correlation");
}

void TestSafeRecovery()
{
	Engine engine;
	Enable(&engine, "batch message-tags server-time draft/chathistory");
	engine.PrepareOutgoing("JOIN #room\r\n");
	const auto recovery = engine.RecoveryCommands(50);
	Check(recovery == std::vector<std::string>{
		"JOIN #room\r\n",
		"CHATHISTORY LATEST #room * 50\r\n",
	}, "recovery rejoins then requests bounded history without resending chat");
}

void TestPlainAndExternal()
{
	Engine implicit_external;
	implicit_external.BeginRegistration({}, "Alice", true);
	auto implicit_offer = implicit_external.Process(":server CAP * LS :sasl=EXTERNAL\r\n");
	Check(RequestedNames(implicit_offer.outbound).empty(),
		"EXTERNAL is never selected without explicit client-certificate opt-in");

	Engine plain;
	SaslConfig credentials;
	credentials.authentication_id = "user";
	credentials.password = std::string(350, 'p');
	credentials.allow_external = false;
	plain.BeginRegistration(credentials, "Alice", true);
	auto request = plain.Process(":server CAP * LS :sasl=PLAIN\r\n");
	Check(RequestedNames(request.outbound) == std::vector<std::string>{"sasl"}, "PLAIN SASL requested");
	auto start = plain.Process(":server CAP Alice ACK :sasl\r\n");
	Check(start.outbound == std::vector<std::string>{"AUTHENTICATE PLAIN\r\n"}, "PLAIN mechanism starts");
	auto response = plain.Process("AUTHENTICATE +\r\n");
	Check(response.outbound.size() == 2, "long PLAIN response uses exact 400-byte chunks");
	Check(AuthenticatePayload(response.outbound[0]).size() == 400, "first SASL chunk is 400 bytes");
	auto mechanisms = plain.Process(":server 908 Alice SCRAM-SHA-256,PLAIN :available mechanisms\r\n");
	Check(mechanisms.events.size() == 1 && !plain.RegistrationFinished() && mechanisms.outbound.empty(),
		"RPL_SASLMECHS is informational rather than terminal");
	auto success = plain.Process(":server 903 Alice :SASL authentication successful\r\n");
	Check(plain.SaslSucceeded() && plain.SecretsCleared(), "PLAIN terminal success zeroizes secrets");
	Check(success.outbound == std::vector<std::string>{"CAP END\r\n"}, "CAP END waits for SASL terminal result");

	Engine external;
	SaslConfig external_config;
	external_config.authorization_id = "cert-user";
	external_config.allow_external = true;
	external.BeginRegistration(external_config, "Alice", true);
	external.Process(":server CAP * LS :sasl=EXTERNAL\r\n");
	auto external_start = external.Process(":server CAP Alice ACK :sasl\r\n");
	Check(external_start.outbound == std::vector<std::string>{"AUTHENTICATE EXTERNAL\r\n"}, "EXTERNAL selected");
	auto external_payload = external.Process("AUTHENTICATE +\r\n");
	Check(Decode(AuthenticatePayload(external_payload.outbound[0])) == "cert-user", "EXTERNAL authorization id payload");
}

void TestScramSha256Rfc7677()
{
	Engine engine;
	SaslConfig config;
	config.authentication_id = "user";
	config.password = "pencil";
	config.allow_external = false;
	config.nonce = "rOprNGfwEbeRWgbNEkqO";
	engine.BeginRegistration(config, "Alice", true);
	engine.Process(":server CAP * LS :sasl=SCRAM-SHA-256,PLAIN\r\n");
	auto start = engine.Process(":server CAP Alice ACK :sasl\r\n");
	Check(start.outbound == std::vector<std::string>{"AUTHENTICATE SCRAM-SHA-256\r\n"},
		"strongest advertised SASL mechanism selected");
	auto first = engine.Process("AUTHENTICATE +\r\n");
	Check(Decode(AuthenticatePayload(first.outbound[0])) == "n,,n=user,r=rOprNGfwEbeRWgbNEkqO",
		"RFC 7677 client-first message");
	const std::string server_first = "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
		"s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
	auto proof = engine.Process("AUTHENTICATE " + Encode(server_first) + "\r\n");
	Check(Decode(AuthenticatePayload(proof.outbound[0])) ==
		"c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
		"p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=", "RFC 7677 client proof");
	const std::string server_final = "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";
	auto verified = engine.Process("AUTHENTICATE " + Encode(server_final) + "\r\n");
	Check(verified.outbound.empty(), "valid SCRAM server signature accepted");
	auto terminal = engine.Process(":server 903 Alice :SASL authentication successful\r\n");
	Check(engine.SaslSucceeded() && engine.SecretsCleared(), "SCRAM success and zeroization");
	Check(terminal.outbound == std::vector<std::string>{"CAP END\r\n"}, "SCRAM gates CAP END");
}

void TestIndependentWireBounds()
{
	Message payload;
	payload.command = "NOTICE";
	payload.params = {"#x", std::string(499, 'p')};
	payload.params.back()[250] = ' ';
	auto exact_payload = payload.SerializeChecked(false);
	Check(exact_payload && exact_payload->size() == 512, "510-byte IRC payload plus CRLF accepted");
	payload.params.back().push_back('p');
	Check(!payload.SerializeChecked(false), "payload over 510 bytes rejected independently of tags");

	Message client_tags;
	client_tags.command = "PING";
	client_tags.tags.push_back({"a", std::string(4092, 't')});
	Check(client_tags.SerializeChecked().has_value(), "4096-byte client tag section accepted");
	client_tags.tags[0].value->push_back('t');
	Check(!client_tags.SerializeChecked(), "client tag section over 4096 bytes rejected");

	const std::string maximum_server_tags = "@a=" + std::string(8187, 's') + " ";
	Check(Message::Parse(maximum_server_tags + "PING\r\n").has_value(), "8191-byte server tag section accepted");
	Check(!Message::Parse("@a=" + std::string(8188, 's') + " PING\r\n"),
		"server tag section over 8191 bytes rejected");
	const std::string maximum_combined = maximum_server_tags + "NOTICE #x :" + std::string(499, 'p') + "\r\n";
	Check(Message::Parse(maximum_combined).has_value(), "maximum tag and payload sections coexist independently");
	LineFramer framer;
	std::vector<std::string> lines;
	Check(framer.Push(maximum_combined, &lines) && lines.size() == 1,
		"default stream framer admits the full legal tagged line");

	Engine engine;
	Check(!engine.PrepareOutgoingChecked(maximum_server_tags + "PING\r\n"),
		"outgoing path enforces the smaller client tag limit");
	Check(!Message::Parse("@bad/key/again=x PING\r\n"), "multi-slash tag key rejected");
	Check(!Message::Parse("@bad..vendor/key=x PING\r\n"), "invalid vendor hostname tag key rejected");
	Check(!Message::Parse("@+ PING\r\n"), "empty client-only tag key rejected");
	Check(!Message::Parse("PRIV@MSG #room :invalid\r\n"), "invalid IRC command token rejected");
	Check(Message::Parse("FUTURECOMMAND #room :preserved\r\n").has_value(),
		"unknown syntactically valid IRC command preserved");
}

void TestTypedTagSemantics()
{
	const auto parsed = Message::Parse("@+reply=parent;+react=wave;+unreact=old;+typing=active;"
		"+channel-context=#elsewhere;msgid=m123;oper=root;bot TAGMSG #room\r\n");
	Check(parsed.has_value(), "typed IRCv3 tag message parses");
	if (!parsed) return;
	const auto tags = parsed->DecodeTags();
	Check(tags.reply == "parent" && tags.reaction == "wave" && tags.unreaction == "old",
		"reply and reaction tags decode");
	Check(tags.typing == TypingStatus::Active && tags.channel_context == "#elsewhere",
		"typing and channel-context tags decode");
	Check(tags.message_id == "m123" && tags.oper == "root" && tags.bot,
		"msgid, oper, and bot tags decode");
	Engine engine;
	auto result = engine.Process(parsed->Serialize());
	Check(result.messages.size() == 1 && result.events.size() == 3,
		"typed tags remain on the message and produce typing/reaction events");
	Check(std::ranges::count_if(result.events, [](const auto& event) {
		return event.type == EventType::Reaction;
	}) == 2, "react and unreact produce distinct typed events");
}

void TestIsupportCaseMappingAndMonitor()
{
	Engine ascii;
	auto support = ascii.Process(":server 005 Alice CASEMAPPING=ascii MONITOR=100 :are supported\r\n");
	Check(support.events.size() == 2 && ascii.CurrentCaseMapping() == CaseMapping::Ascii,
		"005 applies ASCII case mapping with typed events");
	Check(ascii.MonitorLimit() == 100 && ascii.Isupport().at("MONITOR") == "100",
		"005 MONITOR limit retained");
	ascii.Process(":[Nick]!u@h ACCOUNT account\r\n");
	Check(ascii.Accounts().contains("[nick]"), "ASCII mapping keeps bracket identities distinct");
	ascii.Process(":Bob!u@h ACCOUNT bob-account\r\n");
	ascii.Process(":Bob!u@h NICK Robert\r\n");
	Check(!ascii.Accounts().contains("bob") && ascii.Accounts().at("robert") == "bob-account",
		"nickname changes migrate retained identity state");
	ascii.Process(":Robert!u@h QUIT :gone\r\n");
	Check(!ascii.Accounts().contains("robert"), "QUIT clears retained identity state");

	Engine strict;
	strict.Process(":server 005 Alice CASEMAPPING=strict-rfc1459 :supported\r\n");
	strict.Process(":[Nick]!u@h ACCOUNT brackets\r\n");
	strict.Process(":^Nick!u@h ACCOUNT caret\r\n");
	Check(strict.Accounts().contains("{nick}") && strict.Accounts().contains("^nick") &&
		!strict.Accounts().contains("~nick"), "strict RFC1459 folds brackets but not caret");

	Engine rfc1459;
	rfc1459.Process(":^Nick!u@h ACCOUNT caret\r\n");
	Check(rfc1459.Accounts().contains("~nick"), "RFC1459 folds caret to tilde");
	rfc1459.Process(":[Nick]!u@h ACCOUNT ambiguous\r\n");
	rfc1459.Process(":server 005 Alice CASEMAPPING=ascii :supported\r\n");
	Check(!rfc1459.Accounts().contains("{nick}"),
		"dynamic mapping changes discard identities that cannot be reindexed unambiguously");

	auto online = ascii.Process(":server 730 Alice :Bob!u@host,Carol!u@host\r\n");
	Check(online.events.size() == 1 && online.events[0].type == EventType::MonitorOnline &&
		ascii.MonitorOnline().size() == 2, "RPL_MONONLINE updates bounded online state");
	auto offline = ascii.Process(":server 731 Alice :Bob!u@host\r\n");
	Check(offline.events[0].type == EventType::MonitorOffline && !ascii.MonitorOnline().contains("bob"),
		"RPL_MONOFFLINE clears online state");
	ascii.Process(":server 732 Alice :Dave,Eve\r\n");
	Check(ascii.MonitorList().contains("dave") && ascii.MonitorList().contains("eve"),
		"RPL_MONLIST retains monitored targets");
	Check(ascii.Process(":server 733 Alice :End of MONITOR list\r\n").events[0].type ==
		EventType::MonitorListEnd, "RPL_ENDOFMONLIST emits typed event");
	auto full = ascii.Process(":server 734 Alice 2 Frank,Grace :Monitor list is full\r\n");
	Check(full.events[0].type == EventType::MonitorListFull && ascii.MonitorLimit() == 2,
		"RPL_MONLISTFULL updates the advertised bound");
}

void TestCapabilityAndSaslRejectionBounds()
{
	Engine oversized_ls;
	SaslConfig none;
	none.allow_external = false;
	oversized_ls.BeginRegistration(none, "Alice", true);
	ProcessResult overflow;
	for (std::size_t index = 0; index < 70; ++index) {
		overflow = oversized_ls.Process(":server CAP * LS * :" + std::string(480, 'x') + "\r\n");
		if (!overflow.events.empty()) break;
	}
	Check(!overflow.events.empty() && overflow.events[0].type == EventType::ProtocolError &&
		overflow.events[0].key == "cap-ls-too-large", "multiline CAP LS accumulator is bounded");

	Engine capability_count;
	capability_count.BeginRegistration(none, "Alice", true);
	std::vector<std::string> names;
	for (std::size_t index = 0; index < 513; ++index) {
		std::string name = "c" + std::to_string(1000 + index);
		names.push_back(std::move(name));
	}
	for (std::size_t cursor = 0; cursor < names.size(); cursor += 50) {
		std::string list;
		const auto end = (std::min)(names.size(), cursor + 50);
		for (std::size_t index = cursor; index < end; ++index) {
			if (!list.empty()) list.push_back(' ');
			list += names[index];
		}
		const bool continued = end != names.size();
		capability_count.Process(":server CAP * LS " + std::string(continued ? "* :" : ":") + list + "\r\n");
	}
	Check(capability_count.IsOffered(names[511]) && !capability_count.IsOffered(names[512]),
		"offered capability table is capped at 512 entries");

	SaslConfig credentials;
	credentials.authentication_id = "user";
	credentials.password = "password";
	credentials.allow_external = true;
	Engine plaintext;
	plaintext.BeginRegistration(credentials, "Alice", false);
	auto insecure_ls = plaintext.Process(":server CAP * LS :sasl=SCRAM-SHA-256,EXTERNAL,PLAIN\r\n");
	Check(RequestedNames(insecure_ls.outbound).empty() && plaintext.RegistrationFinished(),
		"SASL mechanisms are never attempted on plaintext transport");

	Engine chunk;
	chunk.BeginRegistration(credentials, "Alice", true);
	chunk.Process(":server CAP * LS :sasl=PLAIN\r\n");
	chunk.Process(":server CAP Alice ACK :sasl\r\n");
	auto oversized_chunk = chunk.Process("AUTHENTICATE " + std::string(401, 'A') + "\r\n");
	Check(!oversized_chunk.outbound.empty() && oversized_chunk.outbound[0] == "AUTHENTICATE *\r\n",
		"incoming SASL chunks over 400 bytes are aborted");

	Engine aggregate;
	aggregate.BeginRegistration(credentials, "Alice", true);
	aggregate.Process(":server CAP * LS :sasl=PLAIN\r\n");
	aggregate.Process(":server CAP Alice ACK :sasl\r\n");
	ProcessResult aggregate_result;
	for (std::size_t index = 0; index < 41; ++index)
		aggregate_result = aggregate.Process("AUTHENTICATE " + std::string(400, 'A') + "\r\n");
	Check(!aggregate_result.outbound.empty() && aggregate_result.outbound[0] == "AUTHENTICATE *\r\n",
		"aggregate SASL challenge bytes are bounded");

	for (const auto& challenge : {
		std::string("m=required,r=clientNonceServer,s=c2FsdA==,i=4096"),
		std::string("r=clientNonceServer,s=c2FsdA==,i=4095"),
		std::string("r=") + std::string(1025, 'n') + ",s=c2FsdA==,i=4096",
	}) {
		Engine rejected;
		StartScram(&rejected);
		auto response = FeedChallenge(&rejected, challenge);
		Check(!response.outbound.empty() && response.outbound[0] == "AUTHENTICATE *\r\n",
			"invalid SCRAM mandatory extension, iteration floor, or nonce bound is rejected");
	}
}

void TestBatchFamiliesAndGlobalBound()
{
	Engine families;
	families.Process(":server BATCH +split netsplit old.example new.example\r\n");
	families.Process("@batch=split :Bob QUIT :old.example new.example\r\n");
	auto split = families.Process(":server BATCH -split\r\n");
	Check(std::ranges::any_of(split.events, [](const auto& event) {
		return event.type == EventType::Netsplit;
	}), "netsplit batch produces typed lifecycle event");
	families.Process(":server BATCH +join netjoin old.example new.example\r\n");
	families.Process("@batch=join :Bob JOIN #room\r\n");
	auto join = families.Process(":server BATCH -join\r\n");
	Check(std::ranges::any_of(join.events, [](const auto& event) {
		return event.type == EventType::Netjoin;
	}), "netjoin batch produces typed lifecycle event");
	families.Process(":server BATCH +support draft/isupport\r\n");
	families.Process("@batch=support :server 005 Alice CASEMAPPING=ascii MONITOR=50 :supported\r\n");
	auto support = families.Process(":server BATCH -support\r\n");
	Check(families.CurrentCaseMapping() == CaseMapping::Ascii && families.MonitorLimit() == 50 &&
		std::ranges::any_of(support.events, [](const auto& event) {
			return event.type == EventType::Isupport && event.key == "batch";
		}), "extended-isupport batch applies state and emits typed event");
	families.Process(":server BATCH +outer chathistory #room\r\n");
	families.Process("@batch=outer :server BATCH +inner chathistory #room\r\n");
	auto out_of_order = families.Process(":server BATCH -outer\r\n");
	Check(out_of_order.events.size() == 1 && out_of_order.events[0].key == "batch-close-order",
		"closing a parent before its child drops the batch tree safely");
	Check(families.Process(":server BATCH -missing\r\n").events[0].key == "unknown-batch",
		"unknown batch closure is rejected explicitly");

	Engine bounded;
	for (std::size_t batch = 0; batch < 5; ++batch)
		bounded.Process(":server BATCH +b" + std::to_string(batch) + " chathistory #room\r\n");
	for (std::size_t batch = 0; batch < 5; ++batch) {
		for (std::size_t item = 0; item < 26; ++item) {
			bounded.Process("@pad=" + std::string(8100, 'x') + ";batch=b" + std::to_string(batch) +
				" :Bob NOTICE #room :bounded\r\n");
		}
	}
	auto rejected = bounded.Process(":server BATCH -b4\r\n");
	Check(rejected.messages.empty(), "global one-MiB open-batch byte budget drops the overflowing batch");
	auto retained = bounded.Process(":server BATCH -b0\r\n");
	Check(retained.messages.size() == 26, "global batch overflow does not corrupt independent retained batches");
}

void TestRetainedStateBounds()
{
	constexpr std::size_t maximum = 4096;
	Engine engine;
	ProcessResult last;
	for (std::size_t index = 0; index <= maximum; ++index)
		last = engine.Process(":account" + std::to_string(index) + " ACCOUNT value\r\n");
	Check(engine.Accounts().size() == maximum && last.events[0].type == EventType::ProtocolError,
		"account state is capped");
	for (std::size_t index = 0; index <= maximum; ++index)
		last = engine.Process(":away" + std::to_string(index) + " AWAY :away\r\n");
	Check(engine.AwayMessages().size() == maximum && last.events[0].type == EventType::ProtocolError,
		"away state is capped");
	for (std::size_t index = 0; index <= maximum; ++index)
		last = engine.Process(":host" + std::to_string(index) + " CHGHOST user host\r\n");
	Check(engine.Hosts().size() == maximum && last.events[0].type == EventType::ProtocolError,
		"host state is capped");
	for (std::size_t index = 0; index <= maximum; ++index)
		last = engine.Process(":real" + std::to_string(index) + " SETNAME :Real Name\r\n");
	Check(engine.Realnames().size() == maximum && last.events[0].type == EventType::ProtocolError,
		"realname state is capped");
	for (std::size_t index = 0; index <= maximum; ++index)
		last = engine.Process(":server MARKREAD #room" + std::to_string(index) + " timestamp=1\r\n");
	Check(engine.ReadMarkers().size() == maximum && last.events[0].type == EventType::ProtocolError,
		"read-marker state is capped");
	for (std::size_t index = 0; index <= maximum; ++index)
		last = engine.Process(":Bob REDACT #room id" + std::to_string(index) + " :reason\r\n");
	Check(engine.RedactedMessageIds().size() == maximum && last.events[0].type == EventType::ProtocolError,
		"redaction state is capped");
	for (std::size_t index = 0; index <= maximum; ++index)
		(void)engine.PrepareOutgoingChecked("JOIN #room" + std::to_string(index) + "\r\n");
	Check(engine.RecoveryCommands(0).size() == maximum,
		"joined-channel recovery state is capped");

	Engine metadata;
	for (std::size_t index = 0; index < 65; ++index)
		last = metadata.Process(":server METADATA Bob key" + std::to_string(index) + " * :value\r\n");
	Check(metadata.Metadata().at("bob").size() == 64 && last.events[0].type == EventType::ProtocolError,
		"metadata per-target state is capped");
	Engine metadata_targets;
	for (std::size_t index = 0; index <= 1024; ++index)
		last = metadata_targets.Process(":server METADATA target" + std::to_string(index) + " key * :value\r\n");
	Check(metadata_targets.Metadata().size() == 1024 && last.events[0].type == EventType::ProtocolError,
		"metadata target state is capped");

	Engine monitor;
	for (std::size_t index = 0; index <= maximum; ++index)
		monitor.Process(":server 732 Alice :nick" + std::to_string(index) + "\r\n");
	Check(monitor.MonitorList().size() == maximum, "monitor target state is capped");
}

void TestServerProfilesAndTargetLimits()
{
	struct Fixture {
		const char* version;
		ServerProfile profile;
	};
	for (const auto& fixture : std::array{
		Fixture{"solanum-1.0", ServerProfile::Solanum},
		Fixture{"UnrealIRCd-6.2.1", ServerProfile::UnrealIRCd},
		Fixture{"u2.10.12.19", ServerProfile::Ircu},
		Fixture{"orochi-0.9", ServerProfile::Orochi},
		Fixture{"InspIRCd-4", ServerProfile::InspIRCd},
	}) {
		Engine engine;
		auto result = engine.Process(std::string(":irc.example 004 Alice irc.example ") +
			fixture.version + " io ov\r\n");
		Check(engine.Identity().profile == fixture.profile &&
			engine.Identity().server_name == "irc.example" &&
			!result.events.empty() && result.events[0].type == EventType::ServerIdentity,
			"004 exposes a typed, behavior-neutral server profile");
	}

	Engine limits;
	limits.Process(":server 005 Alice MAXTARGETS=6 TARGMAX=PRIVMSG:4,NOTICE:2,TAGMSG: :supported\r\n");
	Check(limits.TargetLimit("privmsg") == 4 && limits.TargetLimit("NOTICE") == 2 &&
		limits.TargetLimit("TAGMSG") == 64 && limits.TargetLimit("WHOIS") == 6,
		"TARGMAX overrides the bounded MAXTARGETS fallback");
	limits.Process(":server 005 Alice -TARGMAX -MAXTARGETS :changed\r\n");
	Check(!limits.TargetLimit("PRIVMSG"), "ISUPPORT removals clear target limits");
	limits.Process(":server 005 Alice TARGMAX=PRIVMSG:999,NOTICE:0 :invalid\r\n");
	Check(!limits.TargetLimit("PRIVMSG") && !limits.TargetLimit("NOTICE"),
		"unreasonably large or zero target limits are rejected");

	Engine channel_state;
	channel_state.BeginRegistration({}, "Alice", true);
	(void)channel_state.PrepareOutgoingChecked("JOIN #old\r\n");
	channel_state.Process(":server MARKREAD #old timestamp=1\r\n");
	channel_state.Process(":server METADATA #old avatar * :value\r\n");
	channel_state.Process(":server RENAME #old #new :rename\r\n");
	Check(channel_state.ReadMarkers().contains("#new") && channel_state.Metadata().contains("#new") &&
		!channel_state.ReadMarkers().contains("#old") && !channel_state.Metadata().contains("#old"),
		"RENAME migrates channel-keyed retained state");
	channel_state.Process(":Oper KICK #new Alice :bye\r\n");
	Check(channel_state.RecoveryCommands().empty(), "self KICK removes the channel from recovery state");
}

void TestCapFallbackAndKeepalive()
{
	Engine unsupported;
	unsupported.BeginRegistration({}, "Alice", true);
	auto result = unsupported.Process(":old.example 421 Alice CAP :Unknown command\r\n");
	Check(result.consumed && unsupported.RegistrationFinished() &&
		!unsupported.CapabilityResponseSeen() && result.outbound.empty(),
		"ERR_UNKNOWNCOMMAND CAP completes legacy registration without CAP END");

	Engine ignored;
	ignored.BeginRegistration({}, "Alice", true);
	ignored.FinishRegistrationWithoutCapabilities();
	Check(ignored.RegistrationFinished() && ignored.SecretsCleared(),
		"silent CAP timeout completes registration and clears credentials");
	Engine stalled;
	stalled.BeginRegistration({}, "Alice", true);
	stalled.Process(":server CAP * LS :message-tags\r\n");
	auto timeout = stalled.FinishRegistrationAfterTimeout();
	Check(timeout == std::vector<std::string>{"CAP END\r\n"} && stalled.RegistrationFinished(),
		"a stalled CAP-capable server receives exactly one bounded CAP END fallback");
	Check(stalled.FinishRegistrationAfterTimeout().empty(),
		"CAP timeout fallback cannot emit CAP END twice");

	Engine keepalive;
	auto ping = keepalive.PrepareKeepalivePing();
	Check(ping && ping->starts_with("PING cc-ping-"), "keepalive emits a bounded opaque PING");
	Check(!keepalive.PrepareKeepalivePing(), "a second keepalive expires the unmatched PONG deadline");
	keepalive.Process("PONG :wrong-token\r\n");
	Check(keepalive.KeepaliveOutstanding(), "an unrelated PONG cannot satisfy keepalive");
	const auto token_start = ping->find(' ') + 1;
	const auto token_end = ping->find("\r\n");
	const auto token = ping->substr(token_start, token_end - token_start);
	keepalive.Process("PONG :" + token + "\r\n");
	Check(!keepalive.KeepaliveOutstanding() && keepalive.PrepareKeepalivePing(),
		"a matching PONG clears the keepalive deadline");
}

void TestInboundFloodAdmission()
{
	Engine events;
	ProcessResult suppressed;
	for (std::size_t index = 0; index < 33; ++index)
		suppressed = events.Process(":Bob PRIVMSG #room :message\r\n");
	Check(suppressed.consumed && suppressed.events.size() == 1 &&
		suppressed.events[0].type == EventType::Abuse && suppressed.events[0].key == "event" &&
		events.FloodState().suppressed_events == 1,
		"per-source message floods emit a typed suppression event");

	Engine ctcp;
	for (std::size_t index = 0; index < 9; ++index)
		suppressed = ctcp.Process(":Bob PRIVMSG Alice :\x01VERSION\x01\r\n");
	Check(suppressed.events.size() == 1 && suppressed.events[0].key == "ctcp",
		"CTCP has an independent stricter bucket");

	Engine dcc;
	for (std::size_t index = 0; index < 5; ++index)
		suppressed = dcc.Process(":Bob PRIVMSG Alice :\x01" "DCC SEND file 1 2 3\x01\r\n");
	Check(suppressed.events.size() == 1 && suppressed.events[0].key == "dcc" &&
		dcc.FloodState().suppressed_dcc == 1,
		"DCC offers have an independent abuse bucket");

	Engine sound;
	for (std::size_t index = 0; index < 4; ++index)
		suppressed = sound.Process(":Bob PRIVMSG Alice :\x01SOUND alert.wav\x01\r\n");
	Check(suppressed.events.size() == 1 && suppressed.events[0].key == "sound" &&
		sound.FloodState().suppressed_sound == 1,
		"CTCP SOUND is rate-limited before filesystem or audio dispatch");

	auto check_batched_flood = [](const std::string& payload, std::size_t count,
		std::string_view expected_kind) {
		Engine engine;
		engine.Process(":server BATCH +f chathistory #room\r\n");
		ProcessResult result;
		for (std::size_t index = 0; index < count; ++index)
			result = engine.Process("@batch=f :Bob PRIVMSG #room :" + payload + "\r\n");
		Check(result.consumed && result.events.size() == 1 &&
			result.events[0].type == EventType::Abuse && result.events[0].key == expected_kind,
			"batched chat cannot bypass its specialized inbound flood bucket");
	};
	check_batched_flood(std::string{"\x01"} + "VERSION\x01", 9, "ctcp");
	check_batched_flood(std::string{"\x01"} + "DCC SEND file 1 2 3\x01", 5, "dcc");
	check_batched_flood(std::string{"\x01"} + "SOUND alert.wav\x01", 4, "sound");

	Engine lines;
	bool line_suppressed = false;
	for (std::size_t index = 0; index < 4096 && !line_suppressed; ++index) {
		suppressed = lines.Process(":server UNKNOWN value\r\n");
		line_suppressed = suppressed.events.size() == 1 && suppressed.events[0].key == "line";
	}
	Check(line_suppressed && lines.FloodState().suppressed_lines > 0,
		"global inbound line admission is strictly bounded");
	auto critical = lines.Process("PING :still-critical\r\n");
	Check(critical.outbound == std::vector<std::string>{"PONG still-critical\r\n"},
		"protocol-critical PING bypasses exhausted noncritical admission");
}

void TestServerTranscriptFixtures()
{
	struct Fixture {
		const char* file;
		ServerProfile profile;
		bool cap;
	};
	for (const auto& fixture : std::array{
		Fixture{"solanum.txt", ServerProfile::Solanum, true},
		Fixture{"unrealircd.txt", ServerProfile::UnrealIRCd, true},
		Fixture{"ircu.txt", ServerProfile::Ircu, false},
		Fixture{"orochi.txt", ServerProfile::Orochi, true},
		Fixture{"inspircd.txt", ServerProfile::InspIRCd, true},
	}) {
		std::ifstream input{std::filesystem::path{COMICCHAT_IRC_FIXTURE_DIR} / fixture.file};
		Check(input.good(), "server transcript fixture opens");
		Engine engine;
		SaslConfig sasl;
		sasl.authentication_id = "Alice";
		sasl.password = "secret";
		sasl.allow_external = false;
		engine.BeginRegistration(sasl, "Alice", true);
		bool saw_tagged_message{};
		bool saw_batch_event{};
		bool drove_sasl{};
		std::string line;
		while (std::getline(input, line)) {
			auto result = engine.Process(line + "\r\n");
			if (line.find(" CAP * LS ") != std::string::npos) {
				for (const auto& request : result.outbound) {
					if (!request.starts_with("CAP REQ :")) continue;
					const auto names = request.substr(9, request.size() - 11);
					auto acknowledged = engine.Process(":fixture CAP * ACK :" + names + "\r\n");
					for (const auto& command : acknowledged.outbound) {
						if (!command.starts_with("AUTHENTICATE ")) continue;
						auto challenge = engine.Process("AUTHENTICATE +\r\n");
						Check(!challenge.outbound.empty(), "fixture SASL PLAIN challenge is answered");
						engine.Process(":fixture 903 Alice :SASL success\r\n");
						drove_sasl = true;
					}
				}
			}
			for (const auto& message : result.messages)
				saw_tagged_message = saw_tagged_message || !message.tags.empty();
			for (const auto& event : result.events)
				saw_batch_event = saw_batch_event || event.type == EventType::ChatHistory ||
					event.type == EventType::Netsplit || event.type == EventType::Netjoin;
		}
		Check(engine.Identity().profile == fixture.profile,
			"transcript detects the expected server profile without behavior forks");
		Check(engine.RegistrationFinished(), "transcript reaches bounded registration completion");
		if (fixture.cap) {
			Check(drove_sasl && engine.SaslSucceeded(), "CAP fixture exercises successful SASL");
			Check(saw_tagged_message, "CAP fixture preserves message tags");
			Check(saw_batch_event, "CAP fixture exercises bounded batch delivery");
		} else {
			Check(!engine.CapabilityResponseSeen() && !engine.SaslSucceeded(),
				"ircu fallback completes without CAP or SASL assumptions");
		}
	}
}

void TestMetadata2StateMachine()
{
	Engine engine;
	auto set = engine.Process(":server 761 Alice Bob profile/url * :https://example.test\r\n");
	Check(engine.Metadata().at("bob").at("profile/url") == "https://example.test" &&
		set.events.size() == 1 && set.events[0].context[0] == "visibility=*",
		"RPL_KEYVALUE stores a valid metadata value and visibility");
	auto removed = engine.Process(":server 766 Alice Bob profile/url :key not set\r\n");
	Check(!engine.Metadata().contains("bob") && removed.events[0].detail == "deleted",
		"RPL_KEYNOTSET deletes metadata and updates accounting");
	auto invalid = engine.Process(":server METADATA Bob INVALID_KEY * :value\r\n");
	Check(invalid.events.size() == 1 && invalid.events[0].type == EventType::ProtocolError &&
		!engine.Metadata().contains("bob"), "metadata key grammar is enforced");

	engine.Process(":server 770 Alice profile/url status\r\n");
	Check(engine.MetadataSubscriptions().contains("profile/url") &&
		engine.MetadataSubscriptions().contains("status"), "metadata SUB acknowledgements are retained");
	engine.Process(":server 771 Alice status\r\n");
	Check(!engine.MetadataSubscriptions().contains("status"), "metadata UNSUB acknowledgement removes a key");
	engine.Process(":server BATCH +subs metadata-subs\r\n");
	engine.Process("@batch=subs :server 772 Alice avatar status\r\n");
	auto subs = engine.Process(":server BATCH -subs\r\n");
	Check(engine.MetadataSubscriptions() == std::set<std::string>{"avatar", "status"} &&
		std::ranges::any_of(subs.events, [](const auto& event) {
			return event.type == EventType::Metadata && event.key == "metadata-subs";
		}), "metadata-subs batch atomically replaces the subscription snapshot");

	engine.Process(":server BATCH +metadata metadata Bob\r\n");
	engine.Process("@batch=metadata :server 761 Alice Bob avatar * :comic\r\n");
	auto batch = engine.Process(":server BATCH -metadata\r\n");
	Check(engine.Metadata().at("bob").at("avatar") == "comic" &&
		std::ranges::any_of(batch.events, [](const auto& event) {
			return event.type == EventType::Metadata && event.key == "metadata";
		}), "metadata batch updates retained values before delivery");
	auto retry = engine.Process(":server 774 Alice #large 30\r\n");
	Check(retry.events[0].key == "sync-later" && retry.events[0].value == "30",
		"RPL_METADATASYNCLATER exposes a bounded retry-after");
}

void TestAccountRegistrationAndOutputPolicy()
{
	Engine account;
	account.BeginRegistration({}, "Alice", true);
	auto ls = account.Process(":server CAP * LS :standard-replies "
		"draft/account-registration=min-password-length=8,max-password-length=20\r\n");
	const auto requested = RequestedNames(ls.outbound);
	for (const auto& name : requested)
		account.Process(":server CAP * ACK :" + name + "\r\n");
	std::string password = "pass word";
	auto registration = account.PrepareAccountRegistration("*", "alice@example.test", std::move(password));
	Check(registration && *registration ==
		"REGISTER * alice@example.test :pass word\r\n" && password.empty(),
		"REGISTER builder consumes its password and honors negotiated limits");
	std::string weak = "short";
	Check(!account.PrepareAccountRegistration("*", "*", std::move(weak)) && weak.empty(),
		"invalid registration passwords are still zeroized");
	std::string code = "verify-code";
	auto verify = account.PrepareAccountVerification("*", std::move(code));
	Check(verify && *verify == "VERIFY * verify-code\r\n" && code.empty(),
		"VERIFY builder consumes the verification secret");
	auto pending = account.Process(":server REGISTER VERIFICATION_REQUIRED Alice :check email\r\n");
	Check(pending.events[0].type == EventType::AccountRegistration &&
		pending.events[0].key == "VERIFICATION_REQUIRED", "registration status has a typed event");
	auto success = account.Process(":server VERIFY SUCCESS Alice :verified\r\n");
	Check(success.events[0].type == EventType::AccountRegistration &&
		success.events[0].key == "SUCCESS", "verification success has a typed event");
	auto failure = account.Process(":server FAIL REGISTER WEAK_PASSWORD Alice :too weak\r\n");
	Check(failure.events[0].type == EventType::AccountRegistration &&
		failure.events[0].value == "WEAK_PASSWORD", "registration FAIL is typed");

	Engine policy;
	policy.BeginRegistration({}, "Alice", true);
	auto tags = policy.Process(":server CAP * LS :message-tags\r\n");
	for (const auto& name : RequestedNames(tags.outbound))
		policy.Process(":server CAP * ACK :" + name + "\r\n");
	policy.Process(":server 005 Alice CLIENTTAGDENY=*,-reply UTF8ONLY "
		"draft/ICON=https://example.test/icon.svg :supported\r\n");
	Check(policy.ClientTagAllowed("+reply") && !policy.ClientTagAllowed("+react"),
		"CLIENTTAGDENY catch-all and exemptions control client-only tag availability");
	Check(policy.PrepareOutgoingChecked("@+reply=id PRIVMSG #room :ok\r\n") &&
		!policy.PrepareOutgoingChecked("@+react=id TAGMSG #room\r\n"),
		"blocked client tags cannot be emitted");
	std::string invalid_utf8 = "PRIVMSG #room :";
	invalid_utf8.push_back(static_cast<char>(0xff));
	invalid_utf8 += "\r\n";
	Check(!policy.PrepareOutgoingChecked(invalid_utf8),
		"UTF8ONLY rejects malformed outgoing text");
	Check(policy.NetworkIconUrl() == "https://example.test/icon.svg",
		"draft/ICON is exposed as metadata without fetching remote media");
}

} // namespace

int main()
{
	TestMessageTags();
	TestBoundedFramingAndPong();
	TestCapabilityState();
	TestCapabilityRegistryAndDependencies();
	TestOutboundValidationAndTagPreservation();
	TestBatches();
	TestStateEvents();
	TestLabelsAndEchoes();
	TestSafeRecovery();
	TestPlainAndExternal();
	TestScramSha256Rfc7677();
	TestIndependentWireBounds();
	TestTypedTagSemantics();
	TestIsupportCaseMappingAndMonitor();
	TestCapabilityAndSaslRejectionBounds();
	TestBatchFamiliesAndGlobalBound();
	TestRetainedStateBounds();
	TestServerProfilesAndTargetLimits();
	TestCapFallbackAndKeepalive();
	TestInboundFloodAdmission();
	TestServerTranscriptFixtures();
	TestMetadata2StateMachine();
	TestAccountRegistrationAndOutputPolicy();
	if (failures) {
		std::cerr << failures << " IRCv3 test(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "IRCv3 tests passed\n";
	return EXIT_SUCCESS;
}
