#include <comicchat/net/ircv3.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <mbedtls/base64.h>

using comic_chat::ircv3::Engine;
using comic_chat::ircv3::EventType;
using comic_chat::ircv3::LineFramer;
using comic_chat::ircv3::Message;
using comic_chat::ircv3::SaslConfig;

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

void TestMessageTags()
{
	Message message;
	Check(Message::Parse("@aaa=hello\\sworld;semi=x\\:y;empty=;flag;odd=one\\qtwo;drop=end\\;Case=upper;case=lower;dup=one;dup=two;=odd "
		":nick!u@h PRIVMSG #room :hello there\r\n", &message), "parse tagged message");
	Check(message.tags.size() == 10, "all final tag keys retained");
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
	Check(message.FindTag("") && message.FindTag("")->value == "odd", "unknown tag key is not rejected");
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
	auto expanded_ls = expanded.Process(":server CAP * LS :message-tags standard-replies "
		"draft/account-registration draft/extended-isupport draft/oper-tag draft/pre-away extended-monitor\r\n");
	const auto expanded_requested = RequestedNames(expanded_ls.outbound);
	for (const auto* name : {"draft/account-registration", "draft/extended-isupport", "draft/oper-tag",
		"draft/pre-away", "extended-monitor"})
		Check(std::find(expanded_requested.begin(), expanded_requested.end(), name) != expanded_requested.end(),
			"published client capability requested");
	Engine case_sensitive;
	case_sensitive.BeginRegistration(config, "Alice", true);
	auto uppercase = case_sensitive.Process(":server CAP * LS :SASL MESSAGE-TAGS\r\n");
	Check(RequestedNames(uppercase.outbound).empty() && !case_sensitive.IsOffered("sasl"),
		"capability identifiers are case-sensitive opaque names");
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
		engine.Process("@batch=bounded :Bob NOTICE #room :bounded\r\n");
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

} // namespace

int main()
{
	TestMessageTags();
	TestBoundedFramingAndPong();
	TestCapabilityState();
	TestOutboundValidationAndTagPreservation();
	TestBatches();
	TestStateEvents();
	TestLabelsAndEchoes();
	TestSafeRecovery();
	TestPlainAndExternal();
	TestScramSha256Rfc7677();
	if (failures) {
		std::cerr << failures << " IRCv3 test(s) failed\n";
		return EXIT_FAILURE;
	}
	std::cout << "IRCv3 tests passed\n";
	return EXIT_SUCCESS;
}
