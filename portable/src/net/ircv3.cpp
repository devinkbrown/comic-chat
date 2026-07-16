#include <comicchat/net/ircv3.hpp>
#include <comicchat/crypto_runtime.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

namespace comic_chat::ircv3 {
namespace {

constexpr std::size_t kMaxParams = 15;
constexpr std::size_t kMaxTagSection = 8191;
constexpr std::size_t kSaslChunk = 400;
constexpr unsigned long kMaxScramIterations = 1'000'000;
constexpr std::size_t kMaxOpenBatches = 64;
constexpr std::size_t kMaxBatchDepth = 8;
constexpr std::size_t kMaxBatchMessages = 512;
constexpr std::size_t kMaxBatchWireBytes = 256U * 1024U;
constexpr std::size_t kMaxPendingLabels = 1024;

std::string Lower(std::string_view value)
{
	std::string result(value);
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return result;
}

std::string Upper(std::string_view value)
{
	std::string result(value);
	std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
		return static_cast<char>(std::toupper(ch));
	});
	return result;
}

std::string NickFromPrefix(std::string_view prefix)
{
	const auto bang = prefix.find('!');
	return std::string(prefix.substr(0, bang));
}

void SecureClear(std::string* value)
{
	if (!value) return;
	volatile char* bytes = value->empty() ? nullptr : value->data();
	for (std::size_t i = 0; i < value->size(); ++i) bytes[i] = 0;
	value->clear();
	value->shrink_to_fit();
}

template <std::size_t N>
void SecureClear(std::array<unsigned char, N>* value)
{
	volatile unsigned char* bytes = value->data();
	for (std::size_t i = 0; i < value->size(); ++i) bytes[i] = 0;
}

std::vector<std::string> SplitWords(std::string_view value)
{
	std::vector<std::string> result;
	std::size_t cursor = 0;
	while (cursor < value.size()) {
		while (cursor < value.size() && value[cursor] == ' ') ++cursor;
		if (cursor == value.size()) break;
		const auto end = value.find(' ', cursor);
		result.emplace_back(value.substr(cursor, end - cursor));
		cursor = end == std::string_view::npos ? value.size() : end + 1;
	}
	return result;
}

std::vector<std::string> Split(std::string_view value, char delimiter)
{
	std::vector<std::string> result;
	std::size_t cursor = 0;
	do {
		const auto end = value.find(delimiter, cursor);
		result.emplace_back(value.substr(cursor, end - cursor));
		if (end == std::string_view::npos) break;
		cursor = end + 1;
	} while (cursor <= value.size());
	return result;
}

std::string UnescapeTag(std::string_view value)
{
	std::string result;
	result.reserve(value.size());
	for (std::size_t i = 0; i < value.size(); ++i) {
		if (value[i] != '\\') {
			result.push_back(value[i]);
			continue;
		}
		if (++i == value.size()) break;
		switch (value[i]) {
		case ':': result.push_back(';'); break;
		case 's': result.push_back(' '); break;
		case 'r': result.push_back('\r'); break;
		case 'n': result.push_back('\n'); break;
		case '\\': result.push_back('\\'); break;
		default: result.push_back(value[i]); break;
		}
	}
	return result;
}

std::string EscapeTag(std::string_view value)
{
	std::string result;
	result.reserve(value.size());
	for (const char ch : value) {
		switch (ch) {
		case ';': result += "\\:"; break;
		case ' ': result += "\\s"; break;
		case '\\': result += "\\\\"; break;
		case '\r': result += "\\r"; break;
		case '\n': result += "\\n"; break;
		default: result.push_back(ch); break;
		}
	}
	return result;
}

std::optional<unsigned int> NumericCommand(std::string_view command)
{
	if (command.size() != 3 || !std::isdigit(static_cast<unsigned char>(command[0])) ||
		!std::isdigit(static_cast<unsigned char>(command[1])) ||
		!std::isdigit(static_cast<unsigned char>(command[2]))) return std::nullopt;
	return static_cast<unsigned int>((command[0] - '0') * 100 +
		(command[1] - '0') * 10 + command[2] - '0');
}

std::string Base64Encode(const unsigned char* bytes, std::size_t size)
{
	std::size_t required = 0;
	(void)mbedtls_base64_encode(nullptr, 0, &required, bytes, size);
	std::string result(required, '\0');
	std::size_t written = 0;
	if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(result.data()),
		result.size(), &written, bytes, size) != 0) return {};
	result.resize(written);
	return result;
}

std::string Base64Encode(std::string_view value)
{
	return Base64Encode(reinterpret_cast<const unsigned char*>(value.data()), value.size());
}

bool Base64Decode(std::string_view encoded, std::string* decoded)
{
	std::size_t required = 0;
	const auto* input = reinterpret_cast<const unsigned char*>(encoded.data());
	const int sizing = mbedtls_base64_decode(nullptr, 0, &required, input, encoded.size());
	if (sizing != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && sizing != 0) return false;
	decoded->assign(required, '\0');
	std::size_t written = 0;
	if (mbedtls_base64_decode(reinterpret_cast<unsigned char*>(decoded->data()),
		decoded->size(), &written, input, encoded.size()) != 0) {
		SecureClear(decoded);
		return false;
	}
	decoded->resize(written);
	return true;
}

bool Sha256(std::string_view input, std::array<unsigned char, 32>* output)
{
	if (!comicchat::crypto::initialize_runtime()) return false;
	const auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	return info && mbedtls_md(info,
		reinterpret_cast<const unsigned char*>(input.data()), input.size(), output->data()) == 0;
}

bool HmacSha256(
	const unsigned char* key,
	std::size_t key_size,
	std::string_view input,
	std::array<unsigned char, 32>* output)
{
	if (!comicchat::crypto::initialize_runtime()) return false;
	const auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	return info && mbedtls_md_hmac(info, key, key_size,
		reinterpret_cast<const unsigned char*>(input.data()), input.size(), output->data()) == 0;
}

bool ConstantTimeEqual(std::string_view left, std::string_view right)
{
	if (left.size() != right.size()) return false;
	unsigned char difference = 0;
	for (std::size_t i = 0; i < left.size(); ++i)
		difference |= static_cast<unsigned char>(left[i] ^ right[i]);
	return difference == 0;
}

std::string ScramName(std::string_view name)
{
	std::string result;
	for (const char ch : name) {
		if (ch == '=') result += "=3D";
		else if (ch == ',') result += "=2C";
		else result.push_back(ch);
	}
	return result;
}

std::string RandomNonce()
{
	std::array<std::byte, 24> bytes{};
	if (!comicchat::crypto::random_bytes(bytes)) return {};
	return Base64Encode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}

std::vector<std::string> SaslWireChunks(std::string_view encoded)
{
	std::vector<std::string> result;
	if (encoded.empty()) {
		result.emplace_back("AUTHENTICATE +\r\n");
		return result;
	}
	for (std::size_t cursor = 0; cursor < encoded.size(); cursor += kSaslChunk)
		result.emplace_back("AUTHENTICATE " +
			std::string(encoded.substr(cursor, (std::min)(kSaslChunk, encoded.size() - cursor))) + "\r\n");
	if (encoded.size() % kSaslChunk == 0) result.emplace_back("AUTHENTICATE +\r\n");
	return result;
}

struct CapabilityDefinition {
	const char* name;
	std::vector<std::string> dependencies;
};

const std::vector<CapabilityDefinition>& CapabilityCatalog()
{
	static const std::vector<CapabilityDefinition> catalog = {
		{"message-tags", {}},
		{"batch", {}},
		{"server-time", {}},
		{"standard-replies", {}},
		{"cap-notify", {}},
		{"account-notify", {}},
		{"account-tag", {"message-tags"}},
		{"away-notify", {}},
		{"chghost", {}},
		{"echo-message", {}},
		{"extended-join", {"account-notify"}},
		{"invite-notify", {}},
		{"labeled-response", {"batch", "message-tags"}},
		{"multi-prefix", {}},
		{"no-implicit-names", {}},
		{"sasl", {}},
		{"setname", {}},
		{"userhost-in-names", {}},
		{"draft/channel-rename", {}},
		{"draft/account-registration", {"standard-replies"}},
		{"draft/chathistory", {"batch", "server-time", "message-tags"}},
		{"draft/event-playback", {"draft/chathistory"}},
		{"draft/extended-isupport", {}},
		{"draft/metadata-2", {"batch"}},
		{"draft/message-redaction", {"message-tags", "echo-message"}},
		{"draft/multiline", {"batch", "message-tags", "standard-replies"}},
		{"draft/oper-tag", {"message-tags"}},
		{"draft/pre-away", {}},
		{"draft/read-marker", {}},
		{"extended-monitor", {}},
	};
	return catalog;
}

const CapabilityDefinition* FindCapability(std::string_view name)
{
	for (const auto& definition : CapabilityCatalog())
		if (definition.name == name) return &definition;
	return nullptr;
}

} // namespace

bool Message::Parse(std::string_view wire, Message* out, std::string* error)
{
	if (!out) return false;
	while (!wire.empty() && (wire.back() == '\r' || wire.back() == '\n')) wire.remove_suffix(1);
	Message result;
	std::size_t cursor = 0;
		auto fail = [&](const char* reason) {
		if (error) *error = reason;
		return false;
	};
	if (wire.find('\0') != std::string_view::npos || wire.find('\r') != std::string_view::npos ||
		wire.find('\n') != std::string_view::npos) return fail("control byte in IRC line");
	if (!wire.empty() && wire.front() == '@') {
		const auto space = wire.find(' ');
		if (space == std::string_view::npos || space - 1 > kMaxTagSection) return fail("invalid tag section");
		for (const auto& raw : Split(wire.substr(1, space - 1), ';')) {
			const auto equal = raw.find('=');
			const std::string name = raw.substr(0, equal);
			std::optional<std::string> value;
			if (equal != std::string::npos) value = UnescapeTag(std::string_view(raw).substr(equal + 1));
			// Tag keys are case-sensitive opaque identifiers. If duplicated, the
			// final occurrence is authoritative.
			result.SetTag(name, std::move(value));
		}
		cursor = space + 1;
	}
	while (cursor < wire.size() && wire[cursor] == ' ') ++cursor;
	if (cursor < wire.size() && wire[cursor] == ':') {
		const auto space = wire.find(' ', cursor);
		if (space == std::string_view::npos || space == cursor + 1) return fail("invalid prefix");
		result.prefix = std::string(wire.substr(cursor + 1, space - cursor - 1));
		cursor = space + 1;
	}
	while (cursor < wire.size() && wire[cursor] == ' ') ++cursor;
	const auto command_end = wire.find(' ', cursor);
	result.command = Upper(wire.substr(cursor, command_end - cursor));
	if (result.command.empty()) return fail("missing command");
	cursor = command_end == std::string_view::npos ? wire.size() : command_end + 1;
	while (cursor < wire.size()) {
		while (cursor < wire.size() && wire[cursor] == ' ') ++cursor;
		if (cursor == wire.size()) break;
		if (result.params.size() == kMaxParams) return fail("too many parameters");
		if (wire[cursor] == ':') {
			result.params.emplace_back(wire.substr(cursor + 1));
			break;
		}
		const auto end = wire.find(' ', cursor);
		result.params.emplace_back(wire.substr(cursor, end - cursor));
		cursor = end == std::string_view::npos ? wire.size() : end + 1;
	}
	*out = std::move(result);
	return true;
}

std::expected<Message, ParseFailure> Message::Parse(std::string_view wire)
{
	Message message;
	std::string error;
	if (!Parse(wire, &message, &error)) return std::unexpected(ParseFailure{std::move(error)});
	return message;
}

std::string Message::Serialize(bool include_tags) const
{
	std::string result;
	if (include_tags && !tags.empty()) {
		result.push_back('@');
		for (std::size_t i = 0; i < tags.size(); ++i) {
			if (i) result.push_back(';');
			result += tags[i].name;
			if (tags[i].value) {
				result.push_back('=');
				result += EscapeTag(*tags[i].value);
			}
		}
		result.push_back(' ');
	}
	if (prefix) result += ':' + *prefix + ' ';
	result += command;
	for (std::size_t i = 0; i < params.size(); ++i) {
		result.push_back(' ');
		const bool trailing = i + 1 == params.size() &&
			(params[i].empty() || params[i].front() == ':' || params[i].find(' ') != std::string::npos);
		if (trailing) result.push_back(':');
		result += params[i];
	}
	result += "\r\n";
	return result;
}

const Tag* Message::FindTag(std::string_view name) const
{
	for (const auto& tag : tags) if (tag.name == name) return &tag;
	return nullptr;
}

Tag* Message::FindTag(std::string_view name)
{
	for (auto& tag : tags) if (tag.name == name) return &tag;
	return nullptr;
}

void Message::SetTag(std::string name, std::optional<std::string> value)
{
	if (auto* tag = FindTag(name)) {
		tag->value = std::move(value);
		return;
	}
	tags.push_back({std::move(name), std::move(value)});
}

void Message::RemoveTag(std::string_view name)
{
	tags.erase(std::remove_if(tags.begin(), tags.end(), [&](const Tag& tag) {
		return tag.name == name;
	}), tags.end());
}

ProcessResult::~ProcessResult()
{
	for (auto& command : outbound) SecureClear(&command);
}

LineFramer::LineFramer(std::size_t maximum_line_bytes) : maximum_line_bytes_(maximum_line_bytes) {}

std::expected<std::vector<std::string>, std::string> LineFramer::Push(std::span<const std::byte> bytes)
{
	std::vector<std::string> lines;
	std::string error;
	const auto text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	if (!Push(text, &lines, &error)) return std::unexpected(std::move(error));
	return lines;
}

bool LineFramer::Push(std::string_view bytes, std::vector<std::string>* lines, std::string* error)
{
	if (!lines) return false;
	if (bytes.find('\0') != std::string_view::npos) {
		if (error) *error = "NUL in IRC stream";
		Reset();
		return false;
	}
	while (!bytes.empty()) {
		const auto newline = bytes.find('\n');
		const auto length = newline == std::string_view::npos ? bytes.size() : newline + 1;
		const auto allowance = maximum_line_bytes_ + (newline == std::string_view::npos ? 0 : 1);
		if (buffer_.size() + length > allowance) {
			if (error) *error = "IRC line exceeds configured bound";
			Reset();
			return false;
		}
		buffer_.append(bytes.substr(0, length));
		bytes.remove_prefix(length);
		if (newline != std::string_view::npos) {
			lines->push_back(std::move(buffer_));
			buffer_.clear();
		}
	}
	return true;
}

void LineFramer::Reset()
{
	SecureClear(&buffer_);
}

class Engine::SaslSession {
public:
	explicit SaslSession(const SaslConfig& config) : config_(config) {}
	~SaslSession()
	{
		SecureClear(&config_.authentication_id);
		SecureClear(&config_.password);
		SecureClear(&config_.authorization_id);
		SecureClear(&incoming_);
		SecureClear(&client_first_bare_);
		SecureClear(&server_first_);
		SecureClear(&expected_server_signature_);
	}

	std::optional<std::string> Start(std::optional<std::string> advertised)
	{
		std::set<std::string> mechanisms;
		if (advertised && !advertised->empty())
			for (const auto& mechanism : Split(*advertised, ',')) mechanisms.insert(Upper(mechanism));
		auto available = [&](std::string_view mechanism) {
			return mechanisms.empty() || mechanisms.count(std::string(mechanism)) != 0;
		};
		if (!config_.authentication_id.empty() && !config_.password.empty() && available("SCRAM-SHA-256"))
			mechanism_ = "SCRAM-SHA-256";
		else if (config_.allow_external && available("EXTERNAL")) mechanism_ = "EXTERNAL";
		else if (!config_.authentication_id.empty() && !config_.password.empty() && available("PLAIN"))
			mechanism_ = "PLAIN";
		else return std::nullopt;
		return "AUTHENTICATE " + mechanism_ + "\r\n";
	}

	bool Feed(std::string_view chunk, std::vector<std::string>* outbound)
	{
		if (failed_) return false;
		if (chunk != "+") incoming_ += chunk;
		if (chunk.size() == kSaslChunk) return true;
		std::string challenge;
		if (!incoming_.empty() && !Base64Decode(incoming_, &challenge)) return Fail();
		SecureClear(&incoming_);
		const bool ok = Respond(challenge, outbound);
		SecureClear(&challenge);
		return ok;
	}

	bool VerifyTerminalSuccess() const
	{
		return !failed_ && (mechanism_ != "SCRAM-SHA-256" || server_verified_);
	}

private:
	bool Fail() { failed_ = true; return false; }

	bool Respond(std::string_view challenge, std::vector<std::string>* outbound)
	{
		if (mechanism_ == "PLAIN") {
			if (stage_++ != 0 || !challenge.empty()) return Fail();
			std::string plain = config_.authorization_id;
			plain.push_back('\0');
			plain += config_.authentication_id;
			plain.push_back('\0');
			plain += config_.password;
			std::string encoded = Base64Encode(plain);
			*outbound = SaslWireChunks(encoded);
			SecureClear(&plain);
			SecureClear(&encoded);
			return true;
		}
		if (mechanism_ == "EXTERNAL") {
			if (stage_++ != 0 || !challenge.empty()) return Fail();
			*outbound = SaslWireChunks(Base64Encode(config_.authorization_id));
			return true;
		}
		if (mechanism_ != "SCRAM-SHA-256") return Fail();
		if (stage_ == 0) return ScramFirst(challenge, outbound);
		if (stage_ == 1) return ScramProof(challenge, outbound);
		if (stage_ == 2) return ScramFinal(challenge);
		return Fail();
	}

	bool ScramFirst(std::string_view challenge, std::vector<std::string>* outbound)
	{
		if (!challenge.empty()) return Fail();
		nonce_ = config_.nonce.empty() ? RandomNonce() : config_.nonce;
		client_first_bare_ = "n=" + ScramName(config_.authentication_id) + ",r=" + nonce_;
		const std::string gs2 = config_.authorization_id.empty()
			? "n,,"
			: "n,a=" + ScramName(config_.authorization_id) + ',';
		*outbound = SaslWireChunks(Base64Encode(gs2 + client_first_bare_));
		++stage_;
		return true;
	}

	bool ScramProof(std::string_view challenge, std::vector<std::string>* outbound)
	{
		server_first_ = std::string(challenge);
		std::map<std::string, std::string> fields;
		for (const auto& item : Split(challenge, ',')) {
			const auto equal = item.find('=');
			if (equal == std::string::npos || equal == 0 || fields.count(item.substr(0, equal))) return Fail();
			fields[item.substr(0, equal)] = item.substr(equal + 1);
		}
		if (!fields.count("r") || !fields.count("s") || !fields.count("i") ||
			fields["r"].size() <= nonce_.size() || fields["r"].compare(0, nonce_.size(), nonce_) != 0)
			return Fail();
		char* end = nullptr;
		const unsigned long iterations = std::strtoul(fields["i"].c_str(), &end, 10);
		if (!end || *end || iterations == 0 || iterations > kMaxScramIterations) return Fail();
		std::string salt;
		if (!Base64Decode(fields["s"], &salt)) return Fail();
		struct Secrets {
			std::array<unsigned char, 32> salted{};
			std::array<unsigned char, 32> client_key{};
			std::array<unsigned char, 32> stored_key{};
			std::array<unsigned char, 32> client_signature{};
			std::array<unsigned char, 32> server_key{};
			std::array<unsigned char, 32> server_signature{};
			std::array<unsigned char, 32> proof{};
			~Secrets()
			{
				SecureClear(&salted);
				SecureClear(&client_key);
				SecureClear(&stored_key);
				SecureClear(&client_signature);
				SecureClear(&server_key);
				SecureClear(&server_signature);
				SecureClear(&proof);
			}
		} secrets;
		if (mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
			reinterpret_cast<const unsigned char*>(config_.password.data()), config_.password.size(),
			reinterpret_cast<const unsigned char*>(salt.data()), salt.size(),
			static_cast<unsigned int>(iterations), secrets.salted.size(), secrets.salted.data()) != 0) {
			SecureClear(&salt);
			return Fail();
		}
		SecureClear(&salt);
		if (!HmacSha256(secrets.salted.data(), secrets.salted.size(), "Client Key", &secrets.client_key)) return Fail();
		if (!Sha256(std::string_view(reinterpret_cast<const char*>(secrets.client_key.data()),
			secrets.client_key.size()), &secrets.stored_key)) return Fail();
		const std::string final_without_proof = "c=biws,r=" + fields["r"];
		const std::string auth_message = client_first_bare_ + ',' + server_first_ + ',' + final_without_proof;
		if (!HmacSha256(secrets.stored_key.data(), secrets.stored_key.size(), auth_message,
			&secrets.client_signature)) return Fail();
		for (std::size_t i = 0; i < secrets.proof.size(); ++i)
			secrets.proof[i] = secrets.client_key[i] ^ secrets.client_signature[i];
		if (!HmacSha256(secrets.salted.data(), secrets.salted.size(), "Server Key", &secrets.server_key) ||
			!HmacSha256(secrets.server_key.data(), secrets.server_key.size(), auth_message,
				&secrets.server_signature)) return Fail();
		expected_server_signature_ = Base64Encode(secrets.server_signature.data(), secrets.server_signature.size());
		std::string proof_encoded = Base64Encode(secrets.proof.data(), secrets.proof.size());
		std::string final_message = final_without_proof + ",p=" + proof_encoded;
		std::string final_encoded = Base64Encode(final_message);
		*outbound = SaslWireChunks(final_encoded);
		SecureClear(&proof_encoded);
		SecureClear(&final_message);
		SecureClear(&final_encoded);
		++stage_;
		return true;
	}

	bool ScramFinal(std::string_view challenge)
	{
		if (challenge.rfind("e=", 0) == 0 || challenge.rfind("v=", 0) != 0) return Fail();
		server_verified_ = ConstantTimeEqual(challenge.substr(2), expected_server_signature_);
		++stage_;
		return server_verified_ || Fail();
	}

	SaslConfig config_;
	std::string mechanism_;
	std::string incoming_;
	std::string nonce_;
	std::string client_first_bare_;
	std::string server_first_;
	std::string expected_server_signature_;
	unsigned int stage_ = 0;
	bool failed_ = false;
	bool server_verified_ = false;
};

Engine::Engine()
{
	(void)comicchat::crypto::initialize_runtime();
}

Engine::~Engine()
{
	sasl_.reset();
	SecureClear(&sasl_config_.authentication_id);
	SecureClear(&sasl_config_.password);
	SecureClear(&sasl_config_.authorization_id);
}

std::vector<std::string> Engine::BeginRegistration(
	const SaslConfig& sasl,
	std::string nick,
	bool secure_transport)
{
	sasl_.reset();
	SecureClear(&sasl_config_.authentication_id);
	SecureClear(&sasl_config_.password);
	SecureClear(&sasl_config_.authorization_id);
	sasl_config_ = sasl;
	nick_ = std::move(nick);
	secure_transport_ = secure_transport;
	offered_.clear();
	enabled_.clear();
	// CAP LS 302 implicitly enables cap-notify for the connection lifetime.
	enabled_.insert("cap-notify");
	pending_ack_.clear();
	capability_requests_.clear();
	batches_.clear();
	pending_echoes_.clear();
	label_commands_.clear();
	ls_accumulator_.clear();
	cap_negotiating_ = true;
	cap_end_sent_ = false;
	sasl_requested_ = false;
	sasl_terminal_ = false;
	sasl_succeeded_ = false;
	return {"CAP LS 302\r\n"};
}

bool Engine::IsOffered(std::string_view capability) const
{
	return offered_.count(std::string(capability)) != 0;
}

bool Engine::IsEnabled(std::string_view capability) const
{
	return enabled_.count(std::string(capability)) != 0;
}

std::optional<std::string> Engine::CapabilityValue(std::string_view capability) const
{
	const auto found = offered_.find(std::string(capability));
	if (found == offered_.end() || !found->second) return std::nullopt;
	return *found->second;
}

bool Engine::SecretsCleared() const
{
	return sasl_config_.authentication_id.empty() && sasl_config_.password.empty() &&
		sasl_config_.authorization_id.empty();
}

std::vector<std::string> Engine::RecoveryCommands(std::size_t history_limit) const
{
	std::vector<std::string> commands;
	for (const auto& channel : joined_channels_) {
		commands.push_back("JOIN " + channel + "\r\n");
		if (IsEnabled("draft/chathistory"))
			commands.push_back("CHATHISTORY LATEST " + channel + " * " +
				std::to_string(history_limit) + "\r\n");
	}
	return commands;
}

void Engine::ParseCapabilityList(std::string_view list, bool replace)
{
	if (replace) offered_.clear();
	for (auto token : SplitWords(list)) {
		while (!token.empty() && (token.front() == '-' || token.front() == '~' || token.front() == '='))
			token.erase(token.begin());
		const auto equal = token.find('=');
		const auto name = token.substr(0, equal);
		if (name.empty()) continue;
		offered_[name] = equal == std::string::npos
			? std::optional<std::string>{}
			: std::optional<std::string>{token.substr(equal + 1)};
	}
}

bool Engine::DependenciesAvailable(std::string_view name) const
{
	const auto* definition = FindCapability(name);
	if (!definition || !IsOffered(name)) return false;
	for (const auto& dependency : definition->dependencies)
		if (!DependenciesAvailable(dependency)) return false;
	return true;
}

bool Engine::DependenciesEnabled(std::string_view name) const
{
	const auto* definition = FindCapability(name);
	if (!definition) return false;
	for (const auto& dependency : definition->dependencies)
		if (!IsEnabled(dependency)) return false;
	return true;
}

std::vector<std::string> Engine::SelectCapabilities() const
{
	std::vector<std::string> selected;
	for (const auto& definition : CapabilityCatalog()) {
		if (!DependenciesAvailable(definition.name)) continue;
		if (std::string_view(definition.name) == "sasl" &&
			sasl_config_.authentication_id.empty() && !sasl_config_.allow_external) continue;
		selected.emplace_back(definition.name);
	}
	return selected;
}

std::vector<std::string> Engine::RequestCapabilities(const std::vector<std::string>& names)
{
	std::vector<std::string> outbound;
	std::string batch;
	CapabilityRequest request;
	auto flush = [&]() {
		if (batch.empty()) return;
		outbound.push_back("CAP REQ :" + batch + "\r\n");
		capability_requests_.push_back(std::move(request));
		batch.clear();
		request = CapabilityRequest{};
	};
	for (const auto& name : names) {
		if (IsEnabled(name) || pending_ack_.count(name) || !DependenciesAvailable(name)) continue;
		const std::size_t added = name.size() + (batch.empty() ? 0 : 1);
		if (!batch.empty() && batch.size() + added > 400) flush();
		if (!batch.empty()) batch.push_back(' ');
		batch += name;
		request.names.insert(name);
		pending_ack_.insert(name);
		if (name == "sasl") sasl_requested_ = true;
	}
	flush();
	return outbound;
}

void Engine::MaybeFinishRegistration(std::vector<std::string>* outbound)
{
	if (!cap_negotiating_ || cap_end_sent_ || !pending_ack_.empty()) return;
	if (sasl_requested_ && !sasl_terminal_) return;
	outbound->push_back("CAP END\r\n");
	cap_end_sent_ = true;
	cap_negotiating_ = false;
	sasl_.reset();
	SecureClear(&sasl_config_.authentication_id);
	SecureClear(&sasl_config_.password);
	SecureClear(&sasl_config_.authorization_id);
}

void Engine::RemoveCapabilityAndDependents(std::string_view name)
{
	const auto normalized = std::string(name);
	offered_.erase(normalized);
	enabled_.erase(normalized);
	pending_ack_.erase(normalized);
	for (auto& request : capability_requests_) {
		request.names.erase(normalized);
		request.acknowledged.erase(normalized);
	}
	bool changed;
	do {
		changed = false;
		for (const auto& definition : CapabilityCatalog()) {
			if (!enabled_.count(definition.name) && !pending_ack_.count(definition.name)) continue;
			for (const auto& dependency : definition.dependencies) {
				if (!IsEnabled(dependency) && !pending_ack_.count(dependency)) {
					enabled_.erase(definition.name);
					pending_ack_.erase(definition.name);
					changed = true;
					break;
				}
			}
		}
	} while (changed);
	for (auto& request : capability_requests_) {
		std::erase_if(request.names, [&](const std::string& capability) {
			return !pending_ack_.contains(capability);
		});
		std::erase_if(request.acknowledged, [&](const std::string& capability) {
			return !request.names.contains(capability);
		});
	}
	capability_requests_.erase(std::remove_if(capability_requests_.begin(), capability_requests_.end(),
		[](const CapabilityRequest& request) { return request.names.empty(); }), capability_requests_.end());
}

void Engine::UpdateSts(std::optional<std::string> value, std::vector<Event>* events)
{
	if (!value) return;
	StsPolicy policy;
	for (const auto& token : Split(*value, ',')) {
		const auto equal = token.find('=');
		const auto key = Lower(token.substr(0, equal));
		const auto content = equal == std::string::npos ? std::string{} : token.substr(equal + 1);
		char* end = nullptr;
		if (key == "port") {
			const auto port = std::strtoul(content.c_str(), &end, 10);
			if (end && !*end && port > 0 && port <= 65535) policy.port = static_cast<unsigned int>(port);
		} else if (key == "duration") {
			const auto duration = std::strtoull(content.c_str(), &end, 10);
			if (end && !*end) policy.duration = duration;
		} else if (key == "preload") policy.preload = true;
	}
	if (secure_transport_) {
		// A port is an insecure-connection upgrade instruction and has no
		// persistence meaning after TLS is established.
		policy.port.reset();
		if (policy.duration && *policy.duration == 0) sts_policy_.reset();
		else if (policy.duration) sts_policy_ = policy;
	} else {
		// Persistence and preload are trusted only after certificate-verified TLS.
		policy.duration.reset();
		policy.preload = false;
		if (policy.port) sts_policy_ = policy;
	}
	Event event;
	event.type = EventType::StsPolicy;
	event.key = secure_transport_ ? "secure" : "upgrade";
	event.value = *value;
	events->push_back(std::move(event));
}

ProcessResult Engine::HandleCap(const Message& message)
{
	ProcessResult result;
	result.consumed = true;
	std::size_t subcommand_index = message.params.size();
	for (std::size_t i = 0; i < message.params.size(); ++i) {
		const auto value = Upper(message.params[i]);
		if (value == "LS" || value == "ACK" || value == "NAK" || value == "NEW" || value == "DEL") {
			subcommand_index = i;
			break;
		}
	}
	if (subcommand_index == message.params.size()) return result;
	const auto subcommand = Upper(message.params[subcommand_index]);
	const std::string list = message.params.empty() ? std::string{} : message.params.back();
	if (subcommand == "LS") {
		if (!ls_accumulator_.empty() && !list.empty()) ls_accumulator_.push_back(' ');
		ls_accumulator_ += list;
		const bool continued = subcommand_index + 1 < message.params.size() - 1 &&
			message.params[message.params.size() - 2] == "*";
		if (continued) return result;
		ParseCapabilityList(ls_accumulator_, true);
		ls_accumulator_.clear();
		const auto sts = offered_.find("sts");
		if (sts != offered_.end()) UpdateSts(sts->second, &result.events);
		result.outbound = RequestCapabilities(SelectCapabilities());
		if (pending_ack_.empty()) MaybeFinishRegistration(&result.outbound);
		return result;
	}
	if (subcommand == "ACK") {
		std::set<std::string> acknowledged;
		for (const auto& raw : SplitWords(list)) {
			if (raw.empty() || raw.front() == '-') return result;
			acknowledged.insert(raw);
		}
		auto request = std::find_if(capability_requests_.begin(), capability_requests_.end(),
			[&](const CapabilityRequest& candidate) {
				return !acknowledged.empty() && std::ranges::all_of(acknowledged,
					[&](const std::string& name) { return candidate.names.contains(name); });
			});
		if (request == capability_requests_.end()) return result;
		request->acknowledged.insert(acknowledged.begin(), acknowledged.end());
		if (request->acknowledged != request->names) return result;
		for (const auto& name : request->names) {
			pending_ack_.erase(name);
			enabled_.insert(name);
			if (name == "sasl") {
				sasl_ = std::make_unique<SaslSession>(sasl_config_);
				const auto command = sasl_->Start(CapabilityValue("sasl"));
				if (command) result.outbound.push_back(*command);
				else sasl_terminal_ = true;
			}
		}
		capability_requests_.erase(request);
		MaybeFinishRegistration(&result.outbound);
		return result;
	}
	if (subcommand == "NAK") {
		std::set<std::string> rejected;
		for (const auto& raw : SplitWords(list)) {
			const auto name = std::string(raw.front() == '-' ? std::string_view(raw).substr(1) : std::string_view(raw));
			rejected.insert(name);
		}
		auto request = std::find_if(capability_requests_.begin(), capability_requests_.end(),
			[&](const CapabilityRequest& candidate) { return candidate.names == rejected; });
		if (request == capability_requests_.end()) return result;
		for (const auto& name : request->names) {
			pending_ack_.erase(name);
			if (name == "sasl") sasl_terminal_ = true;
		}
		capability_requests_.erase(request);
		MaybeFinishRegistration(&result.outbound);
		return result;
	}
	if (subcommand == "NEW") {
		ParseCapabilityList(list, false);
		const auto sts = offered_.find("sts");
		if (sts != offered_.end()) UpdateSts(sts->second, &result.events);
		result.outbound = RequestCapabilities(SelectCapabilities());
		return result;
	}
	if (subcommand == "DEL") {
		for (const auto& raw : SplitWords(list)) {
			const auto name = raw;
			if (name == "sts" || name == "cap-notify") continue;
			RemoveCapabilityAndDependents(name);
		}
		MaybeFinishRegistration(&result.outbound);
		return result;
	}
	return result;
}

ProcessResult Engine::HandleAuthenticate(const Message& message)
{
	ProcessResult result;
	result.consumed = true;
	if (!sasl_ || message.params.size() != 1 || !sasl_->Feed(message.params[0], &result.outbound)) {
		result.outbound.push_back("AUTHENTICATE *\r\n");
		sasl_terminal_ = true;
		MaybeFinishRegistration(&result.outbound);
	}
	return result;
}

ProcessResult Engine::HandleSaslNumeric(const Message& message, unsigned int numeric)
{
	ProcessResult result;
	result.consumed = numeric >= 900 && numeric <= 908;
	if (!result.consumed) return result;
	if (numeric == 900 || numeric == 901) {
		Event event;
		event.type = EventType::Account;
		event.target = message.params.empty() ? nick_ : message.params[0];
		if (numeric == 900 && message.params.size() >= 3) {
			event.value = message.params[2];
			accounts_[Lower(event.target)] = event.value;
		} else {
			accounts_.erase(Lower(event.target));
		}
		if (!message.params.empty()) event.detail = message.params.back();
		result.events.push_back(std::move(event));
		return result;
	}
	if (numeric == 908) {
		Event event;
		event.type = EventType::StandardReply;
		event.key = "908";
		event.target = "SASL";
		if (message.params.size() >= 2) event.value = message.params[message.params.size() - 2];
		if (!message.params.empty()) event.detail = message.params.back();
		result.events.push_back(std::move(event));
		return result;
	}
	sasl_succeeded_ = numeric == 903 && sasl_ && sasl_->VerifyTerminalSuccess();
	sasl_terminal_ = true;
	sasl_.reset();
	SecureClear(&sasl_config_.authentication_id);
	SecureClear(&sasl_config_.password);
	SecureClear(&sasl_config_.authorization_id);
	MaybeFinishRegistration(&result.outbound);
	return result;
}

std::vector<Message> Engine::FinishBatch(const std::string& id, std::vector<Event>* events)
{
	auto found = batches_.find(id);
	if (found == batches_.end()) return {};
	Batch batch = std::move(found->second);
	batches_.erase(found);
	auto deliver = [&](std::vector<Message> messages) {
		if (batch.label) {
			Event event;
			event.type = EventType::LabeledResponse;
			event.key = *batch.label;
			const auto pending = label_commands_.find(event.key);
			if (pending != label_commands_.end()) {
				event.value = pending->second;
				label_commands_.erase(pending);
			}
			events->push_back(std::move(event));
		}
		if (batch.parent) {
			const auto parent = batches_.find(*batch.parent);
			if (parent != batches_.end()) {
				std::size_t added_bytes = 0;
				for (const auto& message : messages) added_bytes += message.Serialize(true).size();
				if (parent->second.messages.size() + messages.size() > kMaxBatchMessages ||
					parent->second.wire_bytes + added_bytes > kMaxBatchWireBytes) {
					batches_.erase(parent);
					return std::vector<Message>{};
				}
				for (auto& message : messages) message.SetTag("batch", *batch.parent);
				parent->second.wire_bytes += added_bytes;
				parent->second.messages.insert(parent->second.messages.end(),
					std::make_move_iterator(messages.begin()), std::make_move_iterator(messages.end()));
				return std::vector<Message>{};
			}
		}
		for (auto& message : messages) message.RemoveTag("batch");
		return messages;
	};
	if (batch.type == "draft/multiline") {
		if (batch.messages.empty()) return {};
		Message combined = batch.messages.front();
		if (combined.params.size() < 2 || (combined.command != "PRIVMSG" && combined.command != "NOTICE")) return {};
		std::string text = combined.params.back();
		for (std::size_t i = 1; i < batch.messages.size(); ++i) {
			const auto& part = batch.messages[i];
			if (part.command != combined.command || part.params.size() < 2 ||
				part.params[0] != combined.params[0] || part.prefix != combined.prefix) return {};
			if (!part.FindTag("draft/multiline-concat")) text.push_back('\n');
			text += part.params.back();
		}
		combined.params.back() = std::move(text);
		combined.RemoveTag("batch");
		combined.RemoveTag("draft/multiline-concat");
		events->push_back({EventType::Multiline, {}, combined.params[0], {}, combined.params.back(), {}, {}});
		std::vector<Message> messages;
		messages.push_back(std::move(combined));
		return deliver(std::move(messages));
	}
	if (batch.type == "chathistory" || batch.type == "draft/chathistory-targets") {
		Event event;
		event.type = EventType::ChatHistory;
		event.target = batch.params.empty() ? std::string{} : batch.params[0];
		event.value = std::to_string(batch.messages.size());
		events->push_back(std::move(event));
	}
	return deliver(std::move(batch.messages));
}

ProcessResult Engine::HandleBatch(const Message& message)
{
	ProcessResult result;
	result.consumed = true;
	if (message.params.empty() || message.params[0].size() < 2) return result;
	const auto reference = message.params[0];
	const std::string id = reference.substr(1);
	if (reference.front() == '+') {
		if (message.params.size() < 2 || batches_.count(id) || batches_.size() >= kMaxOpenBatches) return result;
		Batch batch;
		batch.type = message.params[1];
		batch.params.assign(message.params.begin() + 2, message.params.end());
		if (const auto* label = message.FindTag("label"); label && label->value) batch.label = *label->value;
		if (const auto* parent = message.FindTag("batch"); parent && parent->value) {
			auto ancestor = batches_.find(*parent->value);
			if (ancestor == batches_.end()) return result;
			batch.parent = *parent->value;
			for (std::size_t depth = 1; ancestor->second.parent; ++depth) {
				if (depth >= kMaxBatchDepth) return result;
				ancestor = batches_.find(*ancestor->second.parent);
				if (ancestor == batches_.end()) return result;
			}
		}
		batches_.emplace(id, std::move(batch));
	} else if (reference.front() == '-') {
		result.messages = FinishBatch(id, &result.events);
		if (!result.messages.empty()) result.consumed = false;
	}
	return result;
}

bool Engine::IsEcho(const Message& message)
{
	if (!IsEnabled("echo-message") || (message.command != "PRIVMSG" && message.command != "NOTICE") ||
		message.params.size() < 2 || !message.prefix || Lower(NickFromPrefix(*message.prefix)) != Lower(nick_))
		return false;
	const auto* label = message.FindTag("label");
	for (auto it = pending_echoes_.begin(); it != pending_echoes_.end(); ++it) {
		const bool label_match = label && label->value && !it->label.empty() && *label->value == it->label;
		const bool content_match = it->command == message.command && it->target == message.params[0] &&
			it->text == message.params.back();
		if (label_match || content_match) {
			label_commands_.erase(it->label);
			pending_echoes_.erase(it);
			return true;
		}
	}
	return false;
}

ProcessResult Engine::HandleStateMessage(const Message& message)
{
	ProcessResult result;
	const std::string source = message.prefix ? NickFromPrefix(*message.prefix) : std::string{};
	auto emit = [&](EventType type) -> Event& {
		result.consumed = true;
		result.events.push_back({});
		auto& event = result.events.back();
		event.type = type;
		event.source = source;
		return event;
	};
	if (message.command == "ACCOUNT" && !message.params.empty()) {
		auto& event = emit(EventType::Account);
		event.value = message.params[0] == "*" ? std::string{} : message.params[0];
		if (event.value.empty()) accounts_.erase(Lower(source)); else accounts_[Lower(source)] = event.value;
	} else if (message.command == "AWAY") {
		auto& event = emit(EventType::Away);
		event.value = message.params.empty() ? std::string{} : message.params.back();
		if (event.value.empty()) away_.erase(Lower(source)); else away_[Lower(source)] = event.value;
	} else if (message.command == "CHGHOST" && message.params.size() >= 2) {
		auto& event = emit(EventType::HostChanged);
		event.key = message.params[0];
		event.value = message.params[1];
		hosts_[Lower(source)] = event.key + '@' + event.value;
	} else if (message.command == "SETNAME" && !message.params.empty()) {
		auto& event = emit(EventType::RealnameChanged);
		event.value = message.params.back();
		realnames_[Lower(source)] = event.value;
	} else if (message.command == "RENAME" && message.params.size() >= 2) {
		auto& event = emit(EventType::ChannelRenamed);
		event.target = message.params[0];
		event.value = message.params[1];
		if (message.params.size() > 2) event.detail = message.params[2];
		if (joined_channels_.erase(Lower(event.target))) joined_channels_.insert(Lower(event.value));
	} else if (message.command == "MARKREAD" && message.params.size() >= 2) {
		auto& event = emit(EventType::ReadMarker);
		event.target = message.params[0];
		event.value = message.params[1];
		read_markers_[Lower(event.target)] = event.value;
	} else if (message.command == "METADATA" && message.params.size() >= 3) {
		auto& event = emit(EventType::Metadata);
		event.target = message.params[0];
		event.key = message.params[1];
		event.value = message.params.back();
		metadata_[Lower(event.target)][event.key] = event.value;
	} else if (message.command == "REDACT" && message.params.size() >= 2) {
		auto& event = emit(EventType::Redaction);
		event.target = message.params[0];
		event.key = message.params[1];
		if (message.params.size() > 2) event.detail = message.params[2];
		redacted_.insert(event.key);
	} else if ((message.command == "FAIL" || message.command == "WARN" || message.command == "NOTE") &&
		message.params.size() >= 3) {
		auto& event = emit(EventType::StandardReply);
		event.key = message.command;
		event.target = message.params[0];
		event.value = message.params[1];
		event.detail = message.params.back();
		event.context.assign(message.params.begin() + 2, message.params.end() - 1);
	}
	return result;
}

ProcessResult Engine::Process(std::string_view wire)
{
	ProcessResult invalid;
	invalid.consumed = true;
	auto parsed = Message::Parse(wire);
	if (!parsed) return invalid;
	Message message = std::move(*parsed);
	if (message.command == "PING") {
		ProcessResult pong;
		pong.consumed = true;
		Message response;
		response.command = "PONG";
		response.params = message.params;
		pong.outbound.push_back(response.Serialize(false));
		return pong;
	}
	if (message.command == "CAP") return HandleCap(message);
	if (message.command == "AUTHENTICATE") return HandleAuthenticate(message);
	if (const auto numeric = NumericCommand(message.command); numeric && *numeric >= 900 && *numeric <= 908)
		return HandleSaslNumeric(message, *numeric);
	if (message.command == "BATCH") return HandleBatch(message);
	if (const auto* batch = message.FindTag("batch"); batch && batch->value) {
		const auto found = batches_.find(*batch->value);
		if (found != batches_.end()) {
			if (found->second.messages.size() >= kMaxBatchMessages ||
				found->second.wire_bytes + wire.size() > kMaxBatchWireBytes) {
				batches_.erase(found);
				ProcessResult result;
				result.consumed = true;
				return result;
			}
			found->second.wire_bytes += wire.size();
			found->second.messages.push_back(std::move(message));
			ProcessResult result;
			result.consumed = true;
			return result;
		}
	}
	ProcessResult result;
	if (const auto* label = message.FindTag("label"); label && label->value) {
		Event event;
		event.type = EventType::LabeledResponse;
		event.key = *label->value;
		const auto pending = label_commands_.find(event.key);
		if (pending != label_commands_.end()) {
			event.value = pending->second;
			label_commands_.erase(pending);
		}
		result.events.push_back(std::move(event));
	}
	if (IsEcho(message)) {
		result.consumed = true;
		return result;
	}
	if (message.prefix && Lower(NickFromPrefix(*message.prefix)) == Lower(nick_)) {
		if (message.command == "JOIN" && !message.params.empty())
			joined_channels_.insert(Lower(message.params[0]));
		else if (message.command == "PART" && !message.params.empty())
			joined_channels_.erase(Lower(message.params[0]));
	}
	auto state = HandleStateMessage(message);
	result.events.insert(result.events.end(),
		std::make_move_iterator(state.events.begin()), std::make_move_iterator(state.events.end()));
	if (state.consumed) {
		result.consumed = true;
		return result;
	}
	result.messages.push_back(std::move(message));
	return result;
}

std::expected<std::string, ParseFailure> Engine::PrepareOutgoingChecked(std::string_view wire)
{
	auto parsed = Message::Parse(wire);
	if (!parsed) return std::unexpected(parsed.error());
	Message message = std::move(*parsed);
	const bool protocol_control = message.command == "CAP" || message.command == "AUTHENTICATE" ||
		message.command == "PASS" || message.command == "NICK" || message.command == "USER" ||
		message.command == "PONG";
	std::string label;
	if (!protocol_control && IsEnabled("labeled-response") && !message.FindTag("label")) {
		std::ostringstream stream;
		stream << "cc" << std::hex << next_label_++;
		label = stream.str();
		message.SetTag("label", label);
		if (label_commands_.size() >= kMaxPendingLabels) label_commands_.erase(label_commands_.begin());
		label_commands_[label] = message.command;
	}
	if (IsEnabled("echo-message") && (message.command == "PRIVMSG" || message.command == "NOTICE") &&
		message.params.size() >= 2) {
		pending_echoes_.push_back({label, message.command, message.params[0], message.params.back()});
		if (pending_echoes_.size() > 128) pending_echoes_.erase(pending_echoes_.begin());
	}
	if (message.command == "JOIN" && !message.params.empty()) {
		for (const auto& channel : Split(message.params[0], ',')) joined_channels_.insert(Lower(channel));
	} else if (message.command == "PART" && !message.params.empty()) {
		for (const auto& channel : Split(message.params[0], ',')) joined_channels_.erase(Lower(channel));
	}
	return message.Serialize(true);
}

std::string Engine::PrepareOutgoing(std::string_view wire)
{
	auto prepared = PrepareOutgoingChecked(wire);
	return prepared ? std::move(*prepared) : std::string{};
}

} // namespace comic_chat::ircv3
