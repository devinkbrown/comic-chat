#include <comicchat/net/ircv3.hpp>
#include <comicchat/crypto_runtime.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>

#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

namespace comic_chat::ircv3 {
namespace {

constexpr std::size_t kMaxParams = 15;
constexpr std::size_t kMaxTagFrameBytes = 8191;
constexpr std::size_t kMaxClientTagFrameBytes = 4096;
constexpr std::size_t kMaxMessageBytes = 510;
constexpr std::size_t kSaslChunk = 400;
constexpr std::size_t kMaxSaslEncodedBytes = 16U * 1024U;
constexpr std::size_t kMaxSaslDecodedBytes = 12U * 1024U;
constexpr std::size_t kMaxScramSaltBytes = 1024;
constexpr std::size_t kMaxScramNonceBytes = 1024;
constexpr std::size_t kMaxScramFields = 16;
constexpr unsigned long kMinScramIterations = 4096;
constexpr unsigned long kMaxScramIterations = 1'000'000;
constexpr std::size_t kMaxOpenBatches = 64;
constexpr std::size_t kMaxBatchDepth = 8;
constexpr std::size_t kMaxBatchMessages = 512;
constexpr std::size_t kMaxBatchWireBytes = 256U * 1024U;
constexpr std::size_t kMaxTotalBatchWireBytes = 1024U * 1024U;
constexpr std::size_t kMaxPendingLabels = 1024;
constexpr std::size_t kMaxCapabilityBytes = 32U * 1024U;
constexpr std::size_t kMaxCapabilities = 512;
constexpr std::size_t kMaxStateEntries = 4096;
constexpr std::size_t kMaxMetadataTargets = 1024;
constexpr std::size_t kMaxMetadataPerTarget = 64;
constexpr std::size_t kMaxMetadataEntries = 4096;
constexpr std::size_t kMaxMetadataSubscriptions = 512;
constexpr std::size_t kMaxIsupportEntries = 512;
constexpr std::size_t kMaxIdentityBytes = 512;
constexpr std::size_t kMaxCredentialBytes = 4096;
constexpr std::size_t kMaxTargetLimitCommands = 32;
constexpr std::size_t kMaxTargetsPerCommand = 64;

bool HasLineControl(std::string_view value)
{
	return value.find('\0') != std::string_view::npos || value.find('\r') != std::string_view::npos ||
		value.find('\n') != std::string_view::npos;
}

bool IsAsciiAlphaNumericHyphen(std::string_view value)
{
	return !value.empty() && std::ranges::all_of(value, [](const unsigned char ch) {
		return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') || ch == '-';
	});
}

bool ValidTagKey(std::string_view key)
{
	if (key.empty()) return false;
	if (key.front() == '+') key.remove_prefix(1);
	if (key.empty()) return false;
	const auto slash = key.find('/');
	if (slash == std::string_view::npos) return IsAsciiAlphaNumericHyphen(key);
	if (slash == 0 || slash + 1 == key.size() || key.find('/', slash + 1) != std::string_view::npos)
		return false;
	const auto vendor = key.substr(0, slash);
	if (!std::ranges::all_of(vendor, [](const unsigned char ch) {
		return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
			(ch >= '0' && ch <= '9') || ch == '-' || ch == '.';
	})) return false;
	std::size_t label_start = 0;
	while (label_start < vendor.size()) {
		const auto label_end = vendor.find('.', label_start);
		const auto label = vendor.substr(label_start, label_end - label_start);
		if (label.empty() || label.front() == '-' || label.back() == '-') return false;
		if (label_end == std::string_view::npos) break;
		label_start = label_end + 1;
	}
	if (vendor.back() == '.') return false;
	return IsAsciiAlphaNumericHyphen(key.substr(slash + 1));
}

bool ValidMetadataKey(std::string_view key)
{
	return !key.empty() && key.size() <= kMaxIdentityBytes &&
		std::ranges::all_of(key, [](const unsigned char ch) {
			return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
				ch == '_' || ch == '.' || ch == '/' || ch == '-';
		});
}

bool ValidCommand(std::string_view command)
{
	if (command.empty()) return false;
	const bool letters = std::ranges::all_of(command, [](const unsigned char ch) {
		return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
	});
	const bool numeric = command.size() == 3 && std::ranges::all_of(command, [](const unsigned char ch) {
		return ch >= '0' && ch <= '9';
	});
	return letters || numeric;
}

bool ValidMiddleParameter(std::string_view value)
{
	return !value.empty() && value.size() <= kMaxIdentityBytes && value.front() != ':' &&
		value.find(' ') == std::string_view::npos && !HasLineControl(value);
}

bool ValidChannelName(std::string_view value)
{
	return ValidMiddleParameter(value) && value.find(',') == std::string_view::npos &&
		value.find('\a') == std::string_view::npos;
}

bool ValidUtf8(std::string_view value)
{
	for (std::size_t offset = 0; offset < value.size();) {
		const auto first = static_cast<unsigned char>(value[offset++]);
		if (first <= 0x7f) continue;
		std::size_t continuation{};
		std::uint32_t codepoint{};
		std::uint32_t minimum{};
		if ((first & 0xe0U) == 0xc0U) {
			continuation = 1; codepoint = first & 0x1fU; minimum = 0x80;
		} else if ((first & 0xf0U) == 0xe0U) {
			continuation = 2; codepoint = first & 0x0fU; minimum = 0x800;
		} else if ((first & 0xf8U) == 0xf0U) {
			continuation = 3; codepoint = first & 0x07U; minimum = 0x10000;
		} else return false;
		if (continuation > value.size() - offset) return false;
		for (std::size_t index = 0; index < continuation; ++index) {
			const auto byte = static_cast<unsigned char>(value[offset++]);
			if ((byte & 0xc0U) != 0x80U) return false;
			codepoint = (codepoint << 6U) | (byte & 0x3fU);
		}
		if (codepoint < minimum || codepoint > 0x10ffffU ||
			(codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
	}
	return true;
}

template <typename Map>
bool BoundedAssign(Map* values, std::string key, std::string value, const std::size_t maximum)
{
	if (key.empty() || key.size() > kMaxIdentityBytes || value.size() > kMaxMessageBytes) return false;
	const auto found = values->find(key);
	if (found == values->end() && values->size() >= maximum) return false;
	(*values)[std::move(key)] = std::move(value);
	return true;
}

template <typename Set>
bool BoundedInsert(Set* values, std::string value, const std::size_t maximum)
{
	if (value.empty() || value.size() > kMaxIdentityBytes) return false;
	if (!values->contains(value) && values->size() >= maximum) return false;
	values->insert(std::move(value));
	return true;
}

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

ServerProfile IdentifyServerProfile(std::string_view version)
{
	const auto normalized = Lower(version);
	if (normalized.find("solanum") != std::string::npos) return ServerProfile::Solanum;
	if (normalized.find("unrealircd") != std::string::npos) return ServerProfile::UnrealIRCd;
	if (normalized.find("orochi") != std::string::npos) return ServerProfile::Orochi;
	if (normalized.find("inspircd") != std::string::npos) return ServerProfile::InspIRCd;
	if (normalized.find("ircu") != std::string::npos || normalized.starts_with("u2."))
		return ServerProfile::Ircu;
	return ServerProfile::Unknown;
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
}

void SecureClear(SaslConfig* value)
{
	if (!value) return;
	SecureClear(&value->authentication_id);
	SecureClear(&value->password);
	SecureClear(&value->authorization_id);
	SecureClear(&value->nonce);
	value->allow_external = false;
}

class ScopedSaslConfigWipe final {
public:
	explicit ScopedSaslConfigWipe(SaslConfig* value) noexcept : value_(value) {}
	~ScopedSaslConfigWipe() { SecureClear(value_); }
	ScopedSaslConfigWipe(const ScopedSaslConfigWipe&) = delete;
	ScopedSaslConfigWipe& operator=(const ScopedSaslConfigWipe&) = delete;
	void Release() noexcept { value_ = nullptr; }

private:
	SaslConfig* value_;
};

void SecureClear(std::vector<std::string>* values)
{
	if (!values) return;
	for (auto& value : *values) SecureClear(&value);
	values->clear();
}

class ScopedStringWipe final {
public:
	explicit ScopedStringWipe(std::string* value) noexcept : value_(value) {}
	~ScopedStringWipe() { SecureClear(value_); }
	ScopedStringWipe(const ScopedStringWipe&) = delete;
	ScopedStringWipe& operator=(const ScopedStringWipe&) = delete;

private:
	std::string* value_;
};

bool IsSensitiveCommand(std::string_view command)
{
	return command == "AUTHENTICATE" || command == "PASS" || command == "AUTH" ||
		command == "OPER" || command == "REGISTER" || command == "VERIFY";
}

void SecureClearMessage(Message* message)
{
	if (!message) return;
	if (message->prefix) SecureClear(&*message->prefix);
	for (auto& tag : message->tags) {
		SecureClear(&tag.name);
		if (tag.value) SecureClear(&*tag.value);
	}
	SecureClear(&message->command);
	for (auto& param : message->params) SecureClear(&param);
}

class ScopedSensitiveMessageWipe final {
public:
	explicit ScopedSensitiveMessageWipe(Message* message) noexcept : message_(message) {}
	~ScopedSensitiveMessageWipe()
	{
		if (message_ && IsSensitiveCommand(message_->command)) SecureClearMessage(message_);
	}

private:
	Message* message_;
};

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

bool Base64Decode(std::string_view encoded, std::string* decoded, const std::size_t maximum)
{
	if (encoded.size() > kMaxSaslEncodedBytes) return false;
	std::size_t required = 0;
	const auto* input = reinterpret_cast<const unsigned char*>(encoded.data());
	const int sizing = mbedtls_base64_decode(nullptr, 0, &required, input, encoded.size());
	if (sizing != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && sizing != 0) return false;
	if (required > maximum) return false;
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
	if (encoded.size() > kMaxSaslEncodedBytes) return result;
	try {
		if (encoded.empty()) {
			result.emplace_back("AUTHENTICATE +\r\n");
			return result;
		}
		for (std::size_t cursor = 0; cursor < encoded.size(); cursor += kSaslChunk)
			result.emplace_back("AUTHENTICATE " +
				std::string(encoded.substr(cursor, (std::min)(kSaslChunk, encoded.size() - cursor))) + "\r\n");
		if (encoded.size() % kSaslChunk == 0) result.emplace_back("AUTHENTICATE +\r\n");
	} catch (...) {
		SecureClear(&result);
		throw;
	}
	return result;
}

struct CapabilityDefinition {
	const char* name;
	std::vector<std::string> dependencies;
	bool auto_request = true;
};

const std::vector<CapabilityDefinition>& CapabilityCatalog()
{
	static const std::vector<CapabilityDefinition> catalog = {
		{"message-tags", {}},
		// Generic batches can carry replayed or shape-changing commands. The
		// product enables this only alongside a complete batch consumer.
		{"batch", {}, false},
		{"server-time", {}},
		{"standard-replies", {}},
		{"cap-notify", {}},
		{"account-notify", {}},
		{"account-tag", {}},
		{"away-notify", {}},
		// Orochi deliberately advertises bot as a capability. The standard bot
		// tag is otherwise enabled by message-tags, so only request this
		// non-standard extension when both are explicitly offered.
		{"bot", {"message-tags"}, false},
		{"chghost", {}},
		// Echo suppression must be driven by a durable pending-message identity,
		// not the current content fallback across multiple bouncer sessions.
		{"echo-message", {}, false},
		// Enable after the Windows adapter normalizes extended JOIN and applies
		// its account and realname fields before the legacy JOIN handler runs.
		{"extended-join", {}, false},
		// Enable after third-party invitations are distinguished from invitations
		// for the local user before reaching the legacy INVITE handler.
		{"invite-notify", {}, false},
		{"labeled-response", {"batch"}, false},
		// Enable after NAMES adaptation decomposes every advertised prefix into
		// legacy user flags instead of treating trailing prefixes as nickname text.
		{"multi-prefix", {}, false},
		// Enable after the JOIN path sends an explicit NAMES request for every
		// joined channel instead of waiting for the server's implicit reply.
		{"no-implicit-names", {}, false},
		{"sasl", {}},
		{"setname", {}},
		// Enable after NAMES adaptation splits nick!user@host and supplies the
		// hostmask separately to the legacy user model.
		{"userhost-in-names", {}, false},
		// Enable after RENAME updates the legacy document, tab, and member model.
		{"draft/channel-rename", {}, false},
		{"draft/account-registration", {}, false},
		// Chathistory has useful limited operation without batch/server-time/
		// message-tags; its specification explicitly tells servers not to
		// enforce those as prerequisites.
		{"draft/chathistory", {}, false},
		// Enable after historical state changes are isolated from live legacy state.
		{"draft/event-playback", {"draft/chathistory"}, false},
		{"draft/extended-isupport", {}, false},
		{"draft/metadata-2", {"batch"}, false},
		// Enable after redactions update already-rendered legacy messages.
		{"draft/message-redaction", {"message-tags"}, false},
		// Enable after receive limits and send-side batching have complete product
		// semantics, including legacy rendering of the reassembled message.
		{"draft/multiline", {"batch"}, false},
		{"draft/oper-tag", {}, false},
		{"draft/pre-away", {}, false},
		{"draft/read-marker", {}, false},
		// Extended-monitor's four notification extensions are independent and
		// optional. Command use remains separately gated by 005 MONITOR.
		{"extended-monitor", {}, false},
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

class Engine::FloodController {
public:
	enum class Kind { Line, Event, Ctcp, Dcc, Sound };

	std::optional<Kind> Admit(const Message& message)
	{
		const auto now = Clock::now();
		const auto numeric = NumericCommand(message.command);
		const bool retained_state = message.command == "ACCOUNT" || message.command == "AWAY" ||
			message.command == "CHGHOST" || message.command == "SETNAME" ||
			message.command == "MARKREAD" || message.command == "METADATA" ||
			message.command == "REDACT" || message.command == "RENAME" ||
			message.command == "REGISTER" || message.command == "VERIFY" ||
			message.command == "FAIL" || message.command == "WARN" || message.command == "NOTE";
		const bool critical = message.command == "PING" || message.command == "PONG" ||
			message.command == "CAP" || message.command == "AUTHENTICATE" ||
			message.command == "BATCH" || message.command == "ERROR" || retained_state ||
			(numeric && ((*numeric >= 730 && *numeric <= 734) ||
				*numeric == 760 || *numeric == 761 || *numeric == 766 ||
				(*numeric >= 770 && *numeric <= 772) || *numeric == 774));
		const bool batch_member = message.FindTag("batch") != nullptr;
		if (!critical && !batch_member && !lines_.Take(now)) {
			++snapshot_.suppressed_lines;
			return Kind::Line;
		}
		++snapshot_.accepted_lines;
		// Batch members bypass only the general line bucket so structural batch
		// delivery remains coherent. Chat inside a batch must still traverse the
		// event, CTCP, DCC and SOUND buckets.
		if (critical || (message.command != "PRIVMSG" && message.command != "NOTICE"))
			return std::nullopt;

		auto& source = Source(message.prefix ? NickFromPrefix(*message.prefix) : std::string{"server"});
		if (!events_.Take(now) || !source.events.Take(now)) {
			++snapshot_.suppressed_events;
			return Kind::Event;
		}
		if (message.params.empty() || message.params.back().empty() ||
			message.params.back().front() != '\x01') return std::nullopt;
		if (!ctcp_.Take(now) || !source.ctcp.Take(now)) {
			++snapshot_.suppressed_ctcp;
			return Kind::Ctcp;
		}

		const auto payload = Upper(message.params.back().substr(1));
		if (payload.starts_with("DCC ") || payload == "DCC") {
			if (!dcc_.Take(now) || !source.dcc.Take(now)) {
				++snapshot_.suppressed_dcc;
				return Kind::Dcc;
			}
		} else if (payload.starts_with("SOUND ") || payload == "SOUND") {
			if (!sound_.Take(now) || !source.sound.Take(now)) {
				++snapshot_.suppressed_sound;
				return Kind::Sound;
			}
		}
		return std::nullopt;
	}

	void Reset()
	{
		lines_ = Bucket{256.0, 512.0};
		events_ = Bucket{64.0, 128.0};
		ctcp_ = Bucket{16.0, 32.0};
		dcc_ = Bucket{2.0, 8.0};
		sound_ = Bucket{2.0, 8.0};
		sources_.clear();
		serial_ = 0;
		snapshot_ = {};
	}

	const FloodSnapshot& Snapshot() const { return snapshot_; }

private:
	using Clock = std::chrono::steady_clock;

	struct Bucket {
		Bucket(double refill = 1.0, double capacity = 1.0)
			: tokens(capacity), rate(refill), burst(capacity), updated(Clock::now()) {}

		bool Take(Clock::time_point now)
		{
			if (now < updated) {
				tokens = burst;
			} else {
				tokens = (std::min)(burst,
					tokens + std::chrono::duration<double>(now - updated).count() * rate);
			}
			updated = now;
			if (tokens < 1.0) return false;
			tokens -= 1.0;
			return true;
		}

		double tokens;
		double rate;
		double burst;
		Clock::time_point updated;
	};

	struct SourceState {
		Bucket events{16.0, 32.0};
		Bucket ctcp{4.0, 8.0};
		Bucket dcc{1.0, 4.0};
		Bucket sound{0.5, 3.0};
		std::uint64_t serial{};
	};

	SourceState& Source(std::string source)
	{
		source = Lower(source.substr(0, kMaxIdentityBytes));
		auto found = sources_.find(source);
		if (found == sources_.end()) {
			if (sources_.size() >= 512) {
				const auto oldest = std::ranges::min_element(sources_, {}, [](const auto& item) {
					return item.second.serial;
				});
				if (oldest != sources_.end()) sources_.erase(oldest);
			}
			found = sources_.try_emplace(std::move(source)).first;
		}
		found->second.serial = ++serial_;
		return found->second;
	}

	Bucket lines_{256.0, 512.0};
	Bucket events_{64.0, 128.0};
	Bucket ctcp_{16.0, 32.0};
	Bucket dcc_{2.0, 8.0};
	Bucket sound_{2.0, 8.0};
	std::map<std::string, SourceState> sources_;
	std::uint64_t serial_{};
	FloodSnapshot snapshot_;
};

bool Message::Parse(std::string_view wire, Message* out, std::string* error)
{
	if (!out) return false;
	if (!wire.empty() && wire.back() == '\n') {
		wire.remove_suffix(1);
		if (!wire.empty() && wire.back() == '\r') wire.remove_suffix(1);
	} else if (!wire.empty() && wire.back() == '\r') {
		wire.remove_suffix(1);
	}
	Message result;
	ScopedSensitiveMessageWipe wipe_result(&result);
	std::size_t cursor = 0;
	auto fail = [&](const char* reason) {
		if (error) *error = reason;
		return false;
	};
	if (HasLineControl(wire)) return fail("control byte in IRC line");
	if (!wire.empty() && wire.front() == '@') {
		const auto space = wire.find(' ');
		if (space == std::string_view::npos || space + 1 > kMaxTagFrameBytes ||
			wire.size() - space - 1 > kMaxMessageBytes) return fail("invalid tag section");
		for (const auto& raw : Split(wire.substr(1, space - 1), ';')) {
			const auto equal = raw.find('=');
			const std::string name = raw.substr(0, equal);
			std::optional<std::string> value;
			if (equal != std::string::npos) value = UnescapeTag(std::string_view(raw).substr(equal + 1));
			// Inbound tag keys are case-sensitive opaque identifiers. IRCv3
			// explicitly forbids rejecting an otherwise valid message because a
			// key does not match today's grammar. Outbound serialization still
			// applies ValidTagKey(). If duplicated, the final occurrence wins.
			result.SetTag(name, std::move(value));
		}
		cursor = space + 1;
	} else if (wire.size() > kMaxMessageBytes) return fail("IRC message exceeds 512-byte frame");
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
	if (!ValidCommand(result.command)) return fail("invalid command");
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

std::expected<std::string, ParseFailure> Message::SerializeChecked(bool include_tags) const
{
	if (!ValidCommand(command))
		return std::unexpected(ParseFailure{"invalid command"});
	if (params.size() > kMaxParams) return std::unexpected(ParseFailure{"too many parameters"});
	std::string tag_frame;
	ScopedStringWipe wipe_tag_frame(IsSensitiveCommand(command) ? &tag_frame : nullptr);
	if (include_tags && !tags.empty()) {
		tag_frame.push_back('@');
		for (std::size_t i = 0; i < tags.size(); ++i) {
			const auto& tag = tags[i];
			if (!ValidTagKey(tag.name)) return std::unexpected(ParseFailure{"invalid tag key"});
			if (i) tag_frame.push_back(';');
			tag_frame += tag.name;
			if (tag.value.has_value()) {
				const auto& value = tag.value.value();
				if (value.find('\0') != std::string::npos)
					return std::unexpected(ParseFailure{"NUL in tag value"});
				tag_frame.push_back('=');
				tag_frame += EscapeTag(value);
			}
		}
		tag_frame.push_back(' ');
		if (tag_frame.size() > kMaxClientTagFrameBytes)
			return std::unexpected(ParseFailure{"tag section exceeds IRCv3 bound"});
	}
	std::string payload;
	ScopedStringWipe wipe_payload(IsSensitiveCommand(command) ? &payload : nullptr);
	if (prefix) {
		if (prefix->empty() || HasLineControl(*prefix) || prefix->find(' ') != std::string::npos)
			return std::unexpected(ParseFailure{"invalid prefix"});
		payload += ':' + *prefix + ' ';
	}
	payload += command;
	for (std::size_t i = 0; i < params.size(); ++i) {
		if (HasLineControl(params[i])) return std::unexpected(ParseFailure{"control byte in parameter"});
		const bool final = i + 1 == params.size();
		if (!final && (params[i].empty() || params[i].front() == ':' ||
			params[i].find(' ') != std::string::npos))
			return std::unexpected(ParseFailure{"invalid middle parameter"});
		payload.push_back(' ');
		const bool trailing = i + 1 == params.size() &&
			(params[i].empty() || params[i].front() == ':' || params[i].find(' ') != std::string::npos);
		if (trailing) payload.push_back(':');
		payload += params[i];
	}
	if (payload.size() > kMaxMessageBytes)
		return std::unexpected(ParseFailure{"IRC message exceeds 512-byte frame"});
	std::string result = std::move(tag_frame);
	result += payload;
	result += "\r\n";
	return result;
}

std::string Message::Serialize(bool include_tags) const
{
	auto serialized = SerializeChecked(include_tags);
	return serialized ? std::move(*serialized) : std::string{};
}

TypedTags Message::DecodeTags() const
{
	TypedTags decoded;
	auto value = [&](std::initializer_list<std::string_view> names) -> std::optional<std::string> {
		for (const auto name : names) {
			if (const auto* tag = FindTag(name); tag && tag->value.has_value() &&
				!tag->value.value().empty())
				return tag->value;
		}
		return std::nullopt;
	};
	decoded.reply = value({"+reply"});
	decoded.reaction = value({"+react", "+draft/react"});
	decoded.unreaction = value({"+unreact", "+draft/unreact"});
	decoded.channel_context = value({"+channel-context", "+draft/channel-context"});
	decoded.message_id = value({"msgid"});
	if (decoded.message_id && (decoded.message_id->front() == ':' || HasLineControl(*decoded.message_id) ||
		decoded.message_id->find(' ') != std::string::npos)) decoded.message_id.reset();
	decoded.oper = value({"oper", "draft/oper"});
	decoded.bot = FindTag("bot") != nullptr;
	if (const auto typing = value({"+typing"}); typing == "active") decoded.typing = TypingStatus::Active;
	else if (typing == "paused") decoded.typing = TypingStatus::Paused;
	else if (typing == "done") decoded.typing = TypingStatus::Done;
	return decoded;
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

ProcessResult::ProcessResult(ProcessResult&& other) noexcept
	: consumed(other.consumed),
	  messages(std::move(other.messages)),
	  outbound(std::move(other.outbound)),
	  events(std::move(other.events)),
	  sts_update(other.sts_update)
{
	other.consumed = false;
	other.sts_update.reset();
}

ProcessResult& ProcessResult::operator=(ProcessResult&& other) noexcept
{
	if (this == &other) return *this;
	SecureClear(&outbound);
	consumed = other.consumed;
	messages = std::move(other.messages);
	outbound = std::move(other.outbound);
	events = std::move(other.events);
	sts_update = other.sts_update;
	other.consumed = false;
	other.sts_update.reset();
	return *this;
}

ProcessResult::~ProcessResult()
{
	SecureClear(&outbound);
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
		if (length > maximum_line_bytes_ || buffer_.size() > maximum_line_bytes_ - length) {
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
	explicit SaslSession(SaslConfig* config) : config_(config) {}
	~SaslSession()
	{
		SecureClear(config_);
		SecureClear(&incoming_);
		SecureClear(&nonce_);
		SecureClear(&client_first_bare_);
		SecureClear(&gs2_header_);
		SecureClear(&server_first_);
		SecureClear(&expected_server_signature_);
	}

	std::optional<std::string> Start(std::optional<std::string> advertised)
	{
		if (config_->authentication_id.size() > kMaxIdentityBytes ||
			config_->authorization_id.size() > kMaxIdentityBytes ||
			config_->password.size() > kMaxCredentialBytes || config_->nonce.size() > kMaxScramNonceBytes ||
			HasLineControl(config_->authentication_id) || HasLineControl(config_->authorization_id) ||
			HasLineControl(config_->password) || HasLineControl(config_->nonce))
			return std::nullopt;
		std::set<std::string> mechanisms;
		if (advertised && !advertised->empty())
			for (const auto& mechanism : Split(*advertised, ',')) mechanisms.insert(Upper(mechanism));
		auto available = [&](std::string_view mechanism) {
			return mechanisms.empty() || mechanisms.count(std::string(mechanism)) != 0;
		};
		if (!config_->authentication_id.empty() && !config_->password.empty() && available("SCRAM-SHA-256"))
			mechanism_ = "SCRAM-SHA-256";
		else if (config_->allow_external && available("EXTERNAL")) mechanism_ = "EXTERNAL";
		else if (!config_->authentication_id.empty() && !config_->password.empty() && available("PLAIN"))
			mechanism_ = "PLAIN";
		else return std::nullopt;
		return "AUTHENTICATE " + mechanism_ + "\r\n";
	}

	bool Feed(std::string_view chunk, std::vector<std::string>* outbound)
	{
		if (failed_ || chunk.empty() || chunk.size() > kSaslChunk || chunk == "*") return Fail();
		if (chunk != "+") {
			if (incoming_.size() > kMaxSaslEncodedBytes - chunk.size()) return Fail();
			incoming_ += chunk;
		}
		if (chunk.size() == kSaslChunk) return true;
		std::string challenge;
		if (!incoming_.empty() && !Base64Decode(incoming_, &challenge, kMaxSaslDecodedBytes)) return Fail();
		SecureClear(&incoming_);
		const bool ok = Respond(challenge, outbound);
		SecureClear(&challenge);
		return ok;
	}

	bool VerifyTerminalSuccess() const
	{
		return !failed_ && (mechanism_ != "SCRAM-SHA-256" || server_verified_);
	}

	bool RequiresScramSignatureVerification() const { return mechanism_ == "SCRAM-SHA-256"; }

private:
	bool Fail() { failed_ = true; return false; }

	bool Respond(std::string_view challenge, std::vector<std::string>* outbound)
	{
		if (mechanism_ == "PLAIN") {
			if (stage_++ != 0 || !challenge.empty()) return Fail();
			std::string plain = config_->authorization_id;
			ScopedStringWipe wipe_plain(&plain);
			plain.push_back('\0');
			plain += config_->authentication_id;
			plain.push_back('\0');
			plain += config_->password;
			std::string encoded = Base64Encode(plain);
			ScopedStringWipe wipe_encoded(&encoded);
			*outbound = SaslWireChunks(encoded);
			const bool valid = !outbound->empty();
			return valid || Fail();
		}
		if (mechanism_ == "EXTERNAL") {
			if (stage_++ != 0 || !challenge.empty()) return Fail();
			std::string encoded = Base64Encode(config_->authorization_id);
			ScopedStringWipe wipe_encoded(&encoded);
			*outbound = SaslWireChunks(encoded);
			return !outbound->empty() || Fail();
		}
		if (mechanism_ != "SCRAM-SHA-256") return Fail();
		if (stage_ == 0) return ScramFirst(challenge, outbound);
		if (stage_ == 1) return ScramProof(challenge, outbound);
		if (stage_ == 2) {
			if (!ScramFinal(challenge)) return false;
			outbound->emplace_back("AUTHENTICATE +\r\n");
			return true;
		}
		return Fail();
	}

	bool ScramFirst(std::string_view challenge, std::vector<std::string>* outbound)
	{
		if (!challenge.empty()) return Fail();
		nonce_ = config_->nonce.empty() ? RandomNonce() : config_->nonce;
		if (nonce_.empty() || nonce_.size() > kMaxScramNonceBytes ||
			!std::ranges::all_of(nonce_, [](const unsigned char ch) {
				return ch >= 0x21U && ch <= 0x7eU && ch != ',';
			})) return Fail();
		client_first_bare_ = "n=" + ScramName(config_->authentication_id) + ",r=" + nonce_;
		// Persisted verbatim so the client-final c= attribute can bind to the
		// exact gs2-header sent here, per RFC 5802 channel-binding rules.
		gs2_header_ = config_->authorization_id.empty()
			? "n,,"
			: "n,a=" + ScramName(config_->authorization_id) + ',';
		std::string first_message = gs2_header_ + client_first_bare_;
		ScopedStringWipe wipe_first_message(&first_message);
		std::string encoded = Base64Encode(first_message);
		ScopedStringWipe wipe_encoded(&encoded);
		*outbound = SaslWireChunks(encoded);
		if (outbound->empty()) return Fail();
		++stage_;
		return true;
	}

	bool ScramProof(std::string_view challenge, std::vector<std::string>* outbound)
	{
		if (challenge.empty() || challenge.size() > kMaxSaslDecodedBytes || HasLineControl(challenge)) return Fail();
		server_first_ = std::string(challenge);
		std::map<std::string, std::string> fields;
		for (const auto& item : Split(challenge, ',')) {
			const auto equal = item.find('=');
			if (equal != 1 || fields.size() >= kMaxScramFields || fields.count(item.substr(0, equal))) return Fail();
			fields[item.substr(0, equal)] = item.substr(equal + 1);
		}
		if (fields.count("m") || !fields.count("r") || !fields.count("s") || !fields.count("i") ||
			fields["r"].size() > kMaxScramNonceBytes || fields["s"].empty() ||
			fields["s"].size() > ((kMaxScramSaltBytes + 2) / 3) * 4 || fields["i"].size() > 10 ||
			fields["r"].size() <= nonce_.size() || fields["r"].compare(0, nonce_.size(), nonce_) != 0)
			return Fail();
		if (!std::ranges::all_of(fields["r"], [](const unsigned char ch) {
			return ch >= 0x21U && ch <= 0x7eU && ch != ',';
		}) || !std::ranges::all_of(fields["i"], [](const unsigned char ch) {
			return ch >= '0' && ch <= '9';
		})) return Fail();
		char* end = nullptr;
		const unsigned long iterations = std::strtoul(fields["i"].c_str(), &end, 10);
		if (!end || *end || iterations < kMinScramIterations || iterations > kMaxScramIterations) return Fail();
		std::string salt;
		if (!Base64Decode(fields["s"], &salt, kMaxScramSaltBytes) || salt.empty()) return Fail();
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
			reinterpret_cast<const unsigned char*>(config_->password.data()), config_->password.size(),
			reinterpret_cast<const unsigned char*>(salt.data()), salt.size(),
			static_cast<unsigned int>(iterations), secrets.salted.size(), secrets.salted.data()) != 0) {
			SecureClear(&salt);
			return Fail();
		}
		SecureClear(&salt);
		if (!HmacSha256(secrets.salted.data(), secrets.salted.size(), "Client Key", &secrets.client_key)) return Fail();
		if (!Sha256(std::string_view(reinterpret_cast<const char*>(secrets.client_key.data()),
			secrets.client_key.size()), &secrets.stored_key)) return Fail();
		std::string channel_binding = Base64Encode(gs2_header_);
		ScopedStringWipe wipe_channel_binding(&channel_binding);
		std::string final_without_proof = "c=" + channel_binding + ",r=" + fields["r"];
		ScopedStringWipe wipe_final_without_proof(&final_without_proof);
		std::string auth_message = client_first_bare_ + ',' + server_first_ + ',' + final_without_proof;
		ScopedStringWipe wipe_auth_message(&auth_message);
		if (!HmacSha256(secrets.stored_key.data(), secrets.stored_key.size(), auth_message,
			&secrets.client_signature)) return Fail();
		for (std::size_t i = 0; i < secrets.proof.size(); ++i)
			secrets.proof[i] = secrets.client_key[i] ^ secrets.client_signature[i];
		if (!HmacSha256(secrets.salted.data(), secrets.salted.size(), "Server Key", &secrets.server_key) ||
			!HmacSha256(secrets.server_key.data(), secrets.server_key.size(), auth_message,
				&secrets.server_signature)) return Fail();
		expected_server_signature_ = Base64Encode(secrets.server_signature.data(), secrets.server_signature.size());
		std::string proof_encoded = Base64Encode(secrets.proof.data(), secrets.proof.size());
		ScopedStringWipe wipe_proof_encoded(&proof_encoded);
		std::string final_message = final_without_proof + ",p=" + proof_encoded;
		ScopedStringWipe wipe_final_message(&final_message);
		std::string final_encoded = Base64Encode(final_message);
		ScopedStringWipe wipe_final_encoded(&final_encoded);
		*outbound = SaslWireChunks(final_encoded);
		const bool valid = !outbound->empty();
		++stage_;
		return valid || Fail();
	}

	bool ScramFinal(std::string_view challenge)
	{
		if (challenge.empty() || challenge.size() > 512 || HasLineControl(challenge)) return Fail();
		std::map<std::string, std::string> fields;
		for (const auto& item : Split(challenge, ',')) {
			const auto equal = item.find('=');
			if (equal != 1 || fields.size() >= kMaxScramFields || fields.count(item.substr(0, equal))) return Fail();
			fields[item.substr(0, equal)] = item.substr(equal + 1);
		}
		if (fields.count("m") || fields.count("e") || fields.size() != 1 || !fields.count("v")) return Fail();
		server_verified_ = ConstantTimeEqual(fields["v"], expected_server_signature_);
		++stage_;
		return server_verified_ || Fail();
	}

	SaslConfig* config_;
	std::string mechanism_;
	std::string incoming_;
	std::string nonce_;
	std::string client_first_bare_;
	std::string gs2_header_;
	std::string server_first_;
	std::string expected_server_signature_;
	unsigned int stage_ = 0;
	bool failed_ = false;
	bool server_verified_ = false;
};

Engine::Engine() : flood_(std::make_unique<FloodController>())
{
	(void)comicchat::crypto::initialize_runtime();
}

Engine::~Engine()
{
	sasl_.reset();
	SecureClear(&sasl_config_.authentication_id);
	SecureClear(&sasl_config_.password);
	SecureClear(&sasl_config_.authorization_id);
	SecureClear(&sasl_config_.nonce);
}

std::vector<std::string> Engine::BeginRegistration(
	SaslConfig&& sasl,
	std::string nick,
	bool secure_transport)
{
	ScopedSaslConfigWipe wipe_source(&sasl);
	return BeginRegistration(static_cast<const SaslConfig&>(sasl), std::move(nick), secure_transport);
}

std::vector<std::string> Engine::BeginRegistration(
	const SaslConfig& sasl,
	std::string nick,
	bool secure_transport)
{
	sasl_.reset();
	SecureClear(&sasl_config_);
	ScopedSaslConfigWipe wipe_destination_on_failure(&sasl_config_);
	sasl_config_ = sasl;
	if (sasl_config_.authentication_id.size() > kMaxIdentityBytes ||
		sasl_config_.authorization_id.size() > kMaxIdentityBytes ||
		sasl_config_.password.size() > kMaxCredentialBytes ||
		sasl_config_.nonce.size() > kMaxScramNonceBytes) {
		SecureClear(&sasl_config_);
	}
	nick_ = std::move(nick);
	secure_transport_ = secure_transport;
	offered_.clear();
	enabled_.clear();
	// CAP LS 302 implicitly enables cap-notify for the connection lifetime.
	enabled_.insert("cap-notify");
	pending_ack_.clear();
	capability_requests_.clear();
	batches_.clear();
	total_batch_wire_bytes_ = 0;
	pending_echoes_.clear();
	label_commands_.clear();
	accounts_.clear();
	away_.clear();
	hosts_.clear();
	realnames_.clear();
	read_markers_.clear();
	metadata_.clear();
	metadata_subscriptions_.clear();
	metadata_entries_ = 0;
	redacted_.clear();
	joined_channels_.clear();
	isupport_.clear();
	monitor_online_.clear();
	monitor_list_.clear();
	monitor_limit_.reset();
	target_limits_.clear();
	max_targets_.reset();
	server_identity_ = {};
	sts_policy_.reset();
	case_mapping_ = CaseMapping::Rfc1459;
	ls_accumulator_.clear();
	keepalive_token_.clear();
	cap_negotiating_ = true;
	cap_end_sent_ = false;
	sasl_requested_ = false;
	sasl_terminal_ = false;
	sasl_succeeded_ = false;
	cap_response_seen_ = false;
	flood_->Reset();
	std::vector<std::string> outbound{"CAP LS 302\r\n"};
	wipe_destination_on_failure.Release();
	return outbound;
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
	return found->second;
}

bool Engine::SetCapabilityRequestEnabled(std::string_view capability, bool enabled)
{
	// cap-notify is an inseparable part of CAP 302 state and STS is observed,
	// never requested. Everything else must be a known client capability.
	if (capability == "cap-notify" || !FindCapability(capability)) return false;
	capability_request_overrides_[std::string(capability)] = enabled;
	return true;
}

std::optional<std::size_t> Engine::TargetLimit(std::string_view command) const
{
	const auto found = target_limits_.find(Upper(command));
	if (found != target_limits_.end()) return found->second;
	return max_targets_;
}

bool Engine::ClientTagAllowed(std::string_view tag) const
{
	if (!tag.empty() && tag.front() == '+') tag.remove_prefix(1);
	const auto found = isupport_.find("CLIENTTAGDENY");
	if (found == isupport_.end() || found->second.empty()) return true;
	bool denied = false;
	for (const auto& item : Split(found->second, ',')) {
		if (!item.empty() && item.front() == '-' && item.substr(1) == tag) {
			denied = false;
		} else if (item == "*" || item == tag) {
			denied = true;
		}
	}
	return !denied;
}

std::optional<std::string> Engine::NetworkIconUrl() const
{
	const auto found = isupport_.find("DRAFT/ICON");
	if (found == isupport_.end() || found->second.empty()) return std::nullopt;
	return found->second;
}

const FloodSnapshot& Engine::FloodState() const
{
	return flood_->Snapshot();
}

bool Engine::SecretsCleared() const
{
	return !sasl_ && sasl_config_.authentication_id.empty() && sasl_config_.password.empty() &&
		sasl_config_.authorization_id.empty() && sasl_config_.nonce.empty();
}

void Engine::FinishRegistrationWithoutCapabilities()
{
	if (cap_end_sent_) return;
	cap_negotiating_ = false;
	cap_end_sent_ = true;
	sasl_requested_ = false;
	sasl_terminal_ = true;
	sasl_.reset();
	pending_ack_.clear();
	capability_requests_.clear();
	enabled_.clear();
	// CAP LS 302 was not accepted, so cap-notify is not implicit.
	SecureClear(&sasl_config_.authentication_id);
	SecureClear(&sasl_config_.password);
	SecureClear(&sasl_config_.authorization_id);
	SecureClear(&sasl_config_.nonce);
}

std::vector<std::string> Engine::FinishRegistrationAfterTimeout()
{
	if (cap_end_sent_) return {};
	const bool server_understands_cap = cap_response_seen_;
	FinishRegistrationWithoutCapabilities();
	return server_understands_cap ? std::vector<std::string>{"CAP END\r\n"} : std::vector<std::string>{};
}

std::expected<std::string, ParseFailure> Engine::PrepareKeepalivePing()
{
	if (!keepalive_token_.empty())
		return std::unexpected(ParseFailure{"keepalive PONG deadline expired"});
	std::ostringstream token;
	token << "cc-ping-" << std::hex << next_keepalive_++;
	keepalive_token_ = token.str();
	Message message;
	message.command = "PING";
	message.params.push_back(keepalive_token_);
	return message.SerializeChecked(false);
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
		if (offered_.size() >= kMaxCapabilities) break;
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
		bool request = definition.auto_request;
		if (const auto override = capability_request_overrides_.find(definition.name);
			override != capability_request_overrides_.end())
			request = override->second;
		if (!request) continue;
		if (!DependenciesAvailable(definition.name)) continue;
		if (std::string_view(definition.name) == "sasl" &&
			(!secure_transport_ || ((!sasl_config_.allow_external) &&
				(sasl_config_.authentication_id.empty() || sasl_config_.password.empty()))))
			continue;
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
	SecureClear(&sasl_config_.nonce);
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

// The outer optional is engaged when STS is present. The inner optional
// preserves the distinction between `sts` and `sts=value`. CAP capability
// lists are processed left-to-right, so a repeated capability's final value
// replaces earlier values; duplicate keys inside that final STS value remain
// invalid in UpdateSts.
static auto UniqueStsValue(const std::string_view list)
	-> std::optional<std::optional<std::string>>
{
	std::optional<std::optional<std::string>> result;
	for (auto token : SplitWords(list)) {
		while (!token.empty() && (token.front() == '-' || token.front() == '~' || token.front() == '='))
			token.erase(token.begin());
		const auto equal = token.find('=');
		if (token.substr(0, equal) != "sts") continue;
		result.emplace(equal == std::string::npos
			? std::optional<std::string>{}
			: std::optional<std::string>{token.substr(equal + 1)});
	}
	return result;
}

std::optional<StsPolicyUpdate> Engine::UpdateSts(
	std::optional<std::string> value, std::vector<Event>* events)
{
	if (!value) return std::nullopt;
	StsPolicy policy;
	bool saw_port = false;
	bool saw_duration = false;
	bool saw_preload = false;
	for (const auto& token : Split(*value, ',')) {
		const auto equal = token.find('=');
		const auto key = token.substr(0, equal);
		const auto content = equal == std::string::npos ? std::string_view{} :
			std::string_view(token).substr(equal + 1);
		if (key == "port") {
			if (saw_port) return std::nullopt;
			saw_port = true;
			if (!secure_transport_) {
				unsigned int port{};
				const auto [end, error] = std::from_chars(
					content.data(), content.data() + content.size(), port);
				if (error != std::errc{} || end != content.data() + content.size() ||
					port == 0 || port > 65'535U) return std::nullopt;
				policy.port = port;
			}
		} else if (key == "duration") {
			if (saw_duration) return std::nullopt;
			saw_duration = true;
			if (secure_transport_) {
				std::uint64_t duration{};
				const auto [end, error] = std::from_chars(
					content.data(), content.data() + content.size(), duration);
				if (error != std::errc{} || end != content.data() + content.size()) return std::nullopt;
				policy.duration = duration;
			}
		} else if (key == "preload") {
			if (saw_preload) return std::nullopt;
			saw_preload = true;
			if (secure_transport_) policy.preload = true;
		}
	}
	std::optional<StsPolicyUpdate> update;
	if (secure_transport_) {
		// A port is an insecure-connection upgrade instruction and has no
		// persistence meaning after TLS is established.
		policy.port.reset();
		if (!policy.duration) return std::nullopt;
		if (*policy.duration == 0) {
			sts_policy_.reset();
			update = StsPolicyUpdate{StsPolicyAction::Remove, 0, 0, false};
		} else {
			sts_policy_ = policy;
			update = StsPolicyUpdate{StsPolicyAction::Persist, 0, *policy.duration, policy.preload};
		}
	} else {
		// Persistence and preload are trusted only after certificate-verified TLS.
		policy.duration.reset();
		policy.preload = false;
		if (!policy.port) return std::nullopt;
		sts_policy_ = policy;
		update = StsPolicyUpdate{
			StsPolicyAction::Upgrade, static_cast<std::uint16_t>(*policy.port), 0, false};
	}
	Event event;
	event.type = EventType::StsPolicy;
	event.key = secure_transport_ ? "secure" : "upgrade";
	event.value = *value;
	events->push_back(std::move(event));
	return update;
}

ProcessResult Engine::HandleCap(const Message& message)
{
	ProcessResult result;
	result.consumed = true;
	// Server CAP replies have the fixed shape CAP <target> <subcommand> ... .
	// Never scan the target for a keyword: a user may legitimately be named LS,
	// ACK, NAK, NEW, or DEL, and confusing it with the subcommand can suppress a
	// security-critical STS advertisement.
	if (message.params.size() < 2) return result;
	constexpr std::size_t subcommand_index = 1;
	const auto subcommand = Upper(message.params[subcommand_index]);
	if (subcommand != "LS" && subcommand != "ACK" && subcommand != "NAK" &&
		subcommand != "NEW" && subcommand != "DEL") return result;
	cap_response_seen_ = true;
	const std::string list = message.params.size() > 2 ? message.params.back() : std::string{};
	if (subcommand == "LS") {
		const std::size_t separator = !ls_accumulator_.empty() && !list.empty() ? 1 : 0;
		if (list.size() > kMaxCapabilityBytes || separator > kMaxCapabilityBytes - list.size() ||
			ls_accumulator_.size() > kMaxCapabilityBytes - list.size() - separator) {
			SecureClear(&ls_accumulator_);
			sasl_requested_ = false;
			sasl_terminal_ = true;
			Event event;
			event.type = EventType::ProtocolError;
			event.key = "cap-ls-too-large";
			result.events.push_back(std::move(event));
			MaybeFinishRegistration(&result.outbound);
			return result;
		}
		if (!ls_accumulator_.empty() && !list.empty()) ls_accumulator_.push_back(' ');
		ls_accumulator_ += list;
		const bool continued = message.params.size() >= 4 && message.params[2] == "*";
		if (continued) return result;
		const auto sts = UniqueStsValue(ls_accumulator_);
		ParseCapabilityList(ls_accumulator_, true);
		ls_accumulator_.clear();
		if (sts) result.sts_update = UpdateSts(*sts, &result.events);
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
		if (request == capability_requests_.end()) {
			if (pending_ack_.contains("sasl")) sasl_terminal_ = true;
			pending_ack_.clear();
			capability_requests_.clear();
			result.events.push_back({EventType::ProtocolError, {}, {}, "cap-ack-invalid", {}, {}, {}});
			MaybeFinishRegistration(&result.outbound);
			return result;
		}
		request->acknowledged.insert(acknowledged.begin(), acknowledged.end());
		if (request->acknowledged != request->names) return result;
		for (const auto& name : request->names) {
			pending_ack_.erase(name);
			enabled_.insert(name);
			if (name == "sasl") {
				if (!secure_transport_) {
					sasl_terminal_ = true;
				} else {
					sasl_ = std::make_unique<SaslSession>(&sasl_config_);
					const auto command = sasl_->Start(CapabilityValue("sasl"));
					if (command) result.outbound.push_back(*command);
					else sasl_terminal_ = true;
				}
			}
		}
		capability_requests_.erase(request);
		MaybeFinishRegistration(&result.outbound);
		return result;
	}
	if (subcommand == "NAK") {
		std::set<std::string> rejected;
		for (const auto& raw : SplitWords(list)) {
			auto name_view = raw.front() == '-' ? std::string_view(raw).substr(1) : std::string_view(raw);
			// Values are not valid in NAK, but stripping one is a safe
			// compatibility path: the capability is still rejected, never enabled.
			name_view = name_view.substr(0, name_view.find('='));
			const auto name = std::string(name_view);
			rejected.insert(name);
		}
		auto request = std::find_if(capability_requests_.begin(), capability_requests_.end(),
			[&](const CapabilityRequest& candidate) { return candidate.names == rejected; });
		if (request == capability_requests_.end()) {
			if (pending_ack_.contains("sasl")) sasl_terminal_ = true;
			pending_ack_.clear();
			capability_requests_.clear();
			result.events.push_back({EventType::ProtocolError, {}, {}, "cap-nak-invalid", {}, {}, {}});
			MaybeFinishRegistration(&result.outbound);
			return result;
		}
		for (const auto& name : request->names) {
			pending_ack_.erase(name);
			if (name == "sasl") sasl_terminal_ = true;
		}
		capability_requests_.erase(request);
		MaybeFinishRegistration(&result.outbound);
		return result;
	}
	if (subcommand == "NEW") {
		const auto sts = UniqueStsValue(list);
		ParseCapabilityList(list, false);
		if (sts) result.sts_update = UpdateSts(*sts, &result.events);
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
	if (!secure_transport_ || !sasl_ || message.params.size() != 1 ||
		!sasl_->Feed(message.params[0], &result.outbound)) {
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
			(void)BoundedAssign(&accounts_, Casefold(event.target), event.value, kMaxStateEntries);
		} else {
			accounts_.erase(Casefold(event.target));
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
	if (numeric == 903 && sasl_ && sasl_->RequiresScramSignatureVerification() &&
		!sasl_->VerifyTerminalSuccess())
		result.events.push_back(
			{EventType::ProtocolError, {}, {}, "sasl-scram-signature-unverified", {}, {}, {}});
	sasl_succeeded_ = numeric == 903 && sasl_ && sasl_->VerifyTerminalSuccess();
	sasl_terminal_ = true;
	sasl_.reset();
	SecureClear(&sasl_config_.authentication_id);
	SecureClear(&sasl_config_.password);
	SecureClear(&sasl_config_.authorization_id);
	SecureClear(&sasl_config_.nonce);
	MaybeFinishRegistration(&result.outbound);
	return result;
}

std::string Engine::Casefold(std::string_view value) const
{
	std::string folded(value);
	for (char& raw : folded) {
		const auto ch = static_cast<unsigned char>(raw);
		if (ch >= 'A' && ch <= 'Z') {
			raw = static_cast<char>(ch + ('a' - 'A'));
			continue;
		}
		if (case_mapping_ == CaseMapping::Ascii) continue;
		if (raw == '[') raw = '{';
		else if (raw == ']') raw = '}';
		else if (raw == '\\') raw = '|';
		else if (raw == '^' && case_mapping_ == CaseMapping::Rfc1459) raw = '~';
	}
	return folded;
}

bool Engine::SameIdentifier(std::string_view left, std::string_view right) const
{
	return Casefold(left) == Casefold(right);
}

void Engine::ReindexState(const CaseMapping previous)
{
	auto ambiguous = [this, previous](std::string_view key) {
		if (case_mapping_ == CaseMapping::Ascii && previous != CaseMapping::Ascii &&
			key.find_first_of("{}|") != std::string_view::npos) return true;
		if (previous == CaseMapping::Rfc1459 && case_mapping_ != CaseMapping::Rfc1459 &&
			key.find('~') != std::string_view::npos) return true;
		return false;
	};
	auto reindex_map = [this](auto* values) {
		std::remove_reference_t<decltype(*values)> reindexed;
		for (auto& [key, value] : *values)
			reindexed[Casefold(key)] = std::move(value);
		*values = std::move(reindexed);
	};
	auto reindex_set = [this](auto* values) {
		std::remove_reference_t<decltype(*values)> reindexed;
		for (const auto& value : *values) reindexed.insert(Casefold(value));
		*values = std::move(reindexed);
	};
	auto discard_ambiguous_map = [&](auto* values) {
		std::erase_if(*values, [&](const auto& entry) { return ambiguous(entry.first); });
	};
	auto discard_ambiguous_set = [&](auto* values) {
		std::erase_if(*values, [&](const auto& value) { return ambiguous(value); });
	};
	discard_ambiguous_map(&accounts_);
	discard_ambiguous_map(&away_);
	discard_ambiguous_map(&hosts_);
	discard_ambiguous_map(&realnames_);
	discard_ambiguous_map(&read_markers_);
	discard_ambiguous_map(&monitor_online_);
	discard_ambiguous_map(&metadata_);
	discard_ambiguous_set(&joined_channels_);
	discard_ambiguous_set(&monitor_list_);
	reindex_map(&accounts_);
	reindex_map(&away_);
	reindex_map(&hosts_);
	reindex_map(&realnames_);
	reindex_map(&read_markers_);
	reindex_map(&monitor_online_);
	{
		decltype(metadata_) reindexed;
		for (auto& [target, entries] : metadata_) {
			auto& destination = reindexed[Casefold(target)];
			for (auto& [key, value] : entries) destination[key] = std::move(value);
		}
		metadata_ = std::move(reindexed);
		metadata_entries_ = 0;
		for (const auto& [target, entries] : metadata_) {
			(void)target;
			metadata_entries_ += entries.size();
		}
	}
	reindex_set(&joined_channels_);
	reindex_set(&monitor_list_);
}

void Engine::HandleIsupport(const Message& message, std::vector<Event>* events)
{
	if (!events || message.command != "005" || message.params.size() < 2) return;
	for (std::size_t index = 1; index + 1 < message.params.size(); ++index) {
		std::string_view raw = message.params[index];
		if (raw.empty()) continue;
		const bool removal = raw.front() == '-';
		if (removal) raw.remove_prefix(1);
		const auto equal = raw.find('=');
		const std::string key = Upper(raw.substr(0, equal));
		if (key.empty() || !std::ranges::all_of(key, [](const unsigned char ch) {
			return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
				ch == '/' || ch == '-' || ch == '_';
		})) continue;
		const std::string value = equal == std::string_view::npos
			? std::string{}
			: std::string(raw.substr(equal + 1));
		if (key.size() > kMaxIdentityBytes || value.size() > kMaxMessageBytes) continue;
		if (key == "CASEMAPPING" && !removal) {
			const auto normalized = Lower(value);
			if (normalized != "ascii" && normalized != "rfc1459" && normalized != "strict-rfc1459")
				continue;
		}
		if (removal) {
			isupport_.erase(key);
		} else if (!BoundedAssign(&isupport_, key, value, kMaxIsupportEntries)) {
			continue;
		}

		if (key == "CASEMAPPING") {
			const auto normalized = Lower(value);
			const auto previous = case_mapping_;
			CaseMapping next = CaseMapping::Rfc1459;
			if (!removal && normalized == "ascii") next = CaseMapping::Ascii;
			else if (!removal && normalized == "strict-rfc1459") next = CaseMapping::StrictRfc1459;
			else if (!removal && normalized != "rfc1459") continue;
			if (next != case_mapping_) {
				case_mapping_ = next;
				ReindexState(previous);
			}
		} else if (key == "MONITOR") {
			monitor_limit_.reset();
			if (!removal && !value.empty()) {
				char* end = nullptr;
				const auto limit = std::strtoull(value.c_str(), &end, 10);
				if (end && !*end && limit <= kMaxStateEntries)
					monitor_limit_ = static_cast<std::size_t>(limit);
			}
		} else if (key == "MAXTARGETS") {
			max_targets_.reset();
			if (!removal && !value.empty()) {
				char* end = nullptr;
				const auto limit = std::strtoull(value.c_str(), &end, 10);
				if (end && !*end && limit > 0 && limit <= kMaxTargetsPerCommand)
					max_targets_ = static_cast<std::size_t>(limit);
			}
		} else if (key == "TARGMAX") {
			target_limits_.clear();
			if (!removal) {
				for (const auto& item : Split(value, ',')) {
					if (target_limits_.size() >= kMaxTargetLimitCommands) break;
					const auto colon = item.find(':');
					const auto command = Upper(item.substr(0, colon));
					if (colon == std::string::npos || !IsAsciiAlphaNumericHyphen(command)) continue;
					std::size_t limit = kMaxTargetsPerCommand;
					if (colon + 1 < item.size()) {
						char* end = nullptr;
						const auto parsed = std::strtoull(item.c_str() + colon + 1, &end, 10);
						if (!end || *end || parsed == 0 || parsed > kMaxTargetsPerCommand) continue;
						limit = static_cast<std::size_t>(parsed);
					}
					target_limits_[command] = limit;
				}
			}
		}

		Event event;
		event.type = EventType::Isupport;
		event.key = key;
		event.value = removal ? std::string{} : value;
		event.detail = removal ? "removed" : "set";
		events->push_back(std::move(event));
	}
}

ProcessResult Engine::HandleMonitorNumeric(const Message& message, unsigned int numeric)
{
	ProcessResult result;
	result.consumed = numeric >= 730 && numeric <= 734;
	if (!result.consumed) return result;
	Event event;
	event.type = numeric == 730 ? EventType::MonitorOnline :
		numeric == 731 ? EventType::MonitorOffline :
		numeric == 732 ? EventType::MonitorList :
		numeric == 733 ? EventType::MonitorListEnd : EventType::MonitorListFull;
	event.key = std::to_string(numeric);

	if (numeric == 733) {
		if (!message.params.empty()) event.detail = message.params.back();
		result.events.push_back(std::move(event));
		return result;
	}
	if (numeric == 734 && message.params.size() >= 2) {
		char* end = nullptr;
		const auto limit = std::strtoull(message.params[1].c_str(), &end, 10);
		if (end && !*end && limit <= kMaxStateEntries)
			monitor_limit_ = static_cast<std::size_t>(limit);
	}
	if (message.params.size() >= 2) {
		const std::size_t list_index = numeric == 734 && message.params.size() >= 3 ? 2 : message.params.size() - 1;
		for (const auto& item : Split(message.params[list_index], ',')) {
			if (item.empty()) continue;
			const std::string nick = NickFromPrefix(item);
			if (nick.empty() || nick.size() > kMaxIdentityBytes) continue;
			event.context.push_back(item);
			const std::string folded = Casefold(nick);
			if (numeric == 730) {
				(void)BoundedAssign(&monitor_online_, folded, item, kMaxStateEntries);
				(void)BoundedInsert(&monitor_list_, folded, kMaxStateEntries);
			} else if (numeric == 731) {
				monitor_online_.erase(folded);
				(void)BoundedInsert(&monitor_list_, folded, kMaxStateEntries);
			} else if (numeric == 732) {
				(void)BoundedInsert(&monitor_list_, folded, kMaxStateEntries);
			}
		}
	}
	if (!message.params.empty()) event.detail = message.params.back();
	result.events.push_back(std::move(event));
	return result;
}

ProcessResult Engine::HandleMetadataNumeric(const Message& message, const unsigned int numeric)
{
	ProcessResult result;
	result.consumed = numeric == 760 || numeric == 761 || numeric == 766 ||
		(numeric >= 770 && numeric <= 772) || numeric == 774;
	if (!result.consumed) return result;

	if ((numeric == 760 || numeric == 761) && message.params.size() >= 5) {
		Message notification;
		notification.prefix = message.prefix;
		notification.command = "METADATA";
		notification.params = {
			message.params[1], message.params[2], message.params[3], message.params[4],
		};
		return HandleStateMessage(notification);
	}
	if (numeric == 766 && message.params.size() >= 4) {
		const auto target = Casefold(message.params[1]);
		const auto& key = message.params[2];
		if (ValidMetadataKey(key)) {
			const auto found = metadata_.find(target);
			if (found != metadata_.end()) {
				if (found->second.erase(key) != 0)
					metadata_entries_ -= (std::min<std::size_t>)(1, metadata_entries_);
				if (found->second.empty()) metadata_.erase(found);
			}
		}
		Event event;
		event.type = EventType::Metadata;
		event.target = message.params[1];
		event.key = key;
		event.detail = "deleted";
		result.events.push_back(std::move(event));
		return result;
	}
	if (numeric >= 770 && numeric <= 772) {
		for (std::size_t index = 1; index < message.params.size(); ++index) {
			const auto& key = message.params[index];
			if (!ValidMetadataKey(key)) continue;
			if (numeric == 771) metadata_subscriptions_.erase(key);
			else (void)BoundedInsert(&metadata_subscriptions_, key, kMaxMetadataSubscriptions);
		}
		Event event;
		event.type = EventType::Metadata;
		event.key = numeric == 770 ? "subscribed" : numeric == 771 ? "unsubscribed" : "subscriptions";
		event.context.assign(message.params.begin() + 1, message.params.end());
		result.events.push_back(std::move(event));
		return result;
	}
	if (numeric == 774 && message.params.size() >= 2) {
		Event event;
		event.type = EventType::Metadata;
		event.key = "sync-later";
		event.target = message.params[1];
		if (message.params.size() >= 3 && message.params[2] != "*") {
			char* end = nullptr;
			const auto retry = std::strtoull(message.params[2].c_str(), &end, 10);
			if (end && !*end && retry > 0 && retry <= 86400) event.value = message.params[2];
		}
		result.events.push_back(std::move(event));
	}
	return result;
}

void Engine::AppendTagEvents(const Message& message, std::vector<Event>* events) const
{
	if (!events) return;
	const auto tags = message.DecodeTags();
	const std::string source = message.prefix ? NickFromPrefix(*message.prefix) : std::string{};
	const std::string target = message.params.empty() ? std::string{} : message.params.front();
	auto append_context = [&](Event* event) {
		if (tags.reply) event->context.push_back("reply=" + *tags.reply);
		if (tags.channel_context) event->context.push_back("channel-context=" + *tags.channel_context);
		if (tags.message_id) event->context.push_back("msgid=" + *tags.message_id);
		if (tags.oper) event->context.push_back("oper=" + *tags.oper);
		if (tags.bot) event->context.push_back("bot");
	};
	if (tags.typing) {
		Event event;
		event.type = EventType::Typing;
		event.source = source;
		event.target = target;
		event.key = *tags.typing == TypingStatus::Active ? "active" :
			*tags.typing == TypingStatus::Paused ? "paused" : "done";
		append_context(&event);
		events->push_back(std::move(event));
	}
	auto append_reaction = [&](const std::optional<std::string>& value, std::string key) {
		if (!value) return;
		Event event;
		event.type = EventType::Reaction;
		event.source = source;
		event.target = target;
		event.key = std::move(key);
		event.value = *value;
		append_context(&event);
		events->push_back(std::move(event));
	};
	append_reaction(tags.reaction, "react");
	append_reaction(tags.unreaction, "unreact");
}

std::vector<Message> Engine::FinishBatch(const std::string& id, std::vector<Event>* events)
{
	auto found = batches_.find(id);
	if (found == batches_.end()) return {};
	if (std::ranges::any_of(batches_, [&](const auto& item) {
		return item.second.parent && *item.second.parent == id;
	})) {
		EraseBatch(id);
		return {};
	}
	Batch batch = std::move(found->second);
	batches_.erase(found);
	auto release_batch_bytes = [&]() {
		if (batch.wire_bytes > total_batch_wire_bytes_) total_batch_wire_bytes_ = 0;
		else total_batch_wire_bytes_ -= batch.wire_bytes;
	};
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
				if (parent->second.messages.size() + messages.size() > kMaxBatchMessages ||
					parent->second.wire_bytes > kMaxBatchWireBytes -
						(std::min)(kMaxBatchWireBytes, batch.wire_bytes)) {
					release_batch_bytes();
					EraseBatch(*batch.parent);
					return std::vector<Message>{};
				}
				for (auto& message : messages) message.SetTag("batch", batch.parent);
				parent->second.wire_bytes += batch.wire_bytes;
				parent->second.messages.insert(parent->second.messages.end(),
					std::make_move_iterator(messages.begin()), std::make_move_iterator(messages.end()));
				return std::vector<Message>{};
			}
		}
		for (auto& message : messages) message.RemoveTag("batch");
		release_batch_bytes();
		return messages;
	};
	if (batch.type == "draft/multiline") {
		if (batch.messages.empty()) {
			release_batch_bytes();
			return {};
		}
		Message combined = batch.messages.front();
		if (combined.params.size() < 2 || (combined.command != "PRIVMSG" && combined.command != "NOTICE")) {
			release_batch_bytes();
			return {};
		}
		std::string text = combined.params.back();
		for (std::size_t i = 1; i < batch.messages.size(); ++i) {
			const auto& part = batch.messages[i];
			if (part.command != combined.command || part.params.size() < 2 ||
				part.params[0] != combined.params[0] || part.prefix != combined.prefix) {
				release_batch_bytes();
				return {};
			}
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
	if (batch.type == "metadata" || batch.type == "metadata-subs") {
		Event summary;
		summary.type = EventType::Metadata;
		summary.key = batch.type;
		summary.target = batch.params.empty() ? std::string{} : batch.params[0];
		summary.value = std::to_string(batch.messages.size());
		events->push_back(std::move(summary));
		for (const auto& item : batch.messages) {
			ProcessResult state;
			if (const auto numeric = NumericCommand(item.command))
				state = HandleMetadataNumeric(item, *numeric);
			else if (item.command == "METADATA")
				state = HandleStateMessage(item);
			events->insert(events->end(),
				std::make_move_iterator(state.events.begin()),
				std::make_move_iterator(state.events.end()));
		}
	}
	if (batch.type == "chathistory" || batch.type == "draft/chathistory-targets") {
		Event event;
		event.type = EventType::ChatHistory;
		event.target = batch.params.empty() ? std::string{} : batch.params[0];
		event.value = std::to_string(batch.messages.size());
		events->push_back(std::move(event));
	}
	if (batch.type == "netsplit" || batch.type == "draft/netsplit" ||
		batch.type == "netjoin" || batch.type == "draft/netjoin") {
		Event event;
		event.type = batch.type.find("netsplit") != std::string::npos
			? EventType::Netsplit : EventType::Netjoin;
		event.context = batch.params;
		event.value = std::to_string(batch.messages.size());
		events->push_back(std::move(event));
	}
	if (batch.type == "isupport" || batch.type == "draft/isupport") {
		Event event;
		event.type = EventType::Isupport;
		event.key = "batch";
		event.context = batch.params;
		event.value = std::to_string(batch.messages.size());
		events->push_back(std::move(event));
		for (const auto& item : batch.messages) HandleIsupport(item, events);
	}
	return deliver(std::move(batch.messages));
}

void Engine::EraseBatch(const std::string& id)
{
	std::vector<std::string> descendants;
	for (const auto& [candidate, batch] : batches_)
		if (batch.parent && *batch.parent == id) descendants.push_back(candidate);
	for (const auto& descendant : descendants) EraseBatch(descendant);
	const auto found = batches_.find(id);
	if (found == batches_.end()) return;
	if (found->second.wire_bytes > total_batch_wire_bytes_) total_batch_wire_bytes_ = 0;
	else total_batch_wire_bytes_ -= found->second.wire_bytes;
	batches_.erase(found);
}

ProcessResult Engine::HandleBatch(const Message& message)
{
	ProcessResult result;
	result.consumed = true;
	if (message.params.empty() || message.params[0].size() < 2) return result;
	const auto reference = message.params[0];
	const std::string id = reference.substr(1);
	if (reference.front() == '+') {
		if (message.params.size() < 2 || batches_.count(id) || batches_.size() >= kMaxOpenBatches) {
			result.events.push_back({EventType::ProtocolError, {}, {}, "batch-open-limit", {}, {}, {}});
			return result;
		}
		Batch batch;
		batch.type = message.params[1];
		if (batch.type == "metadata-subs") metadata_subscriptions_.clear();
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
		if (!batches_.contains(id)) {
			result.events.push_back({EventType::ProtocolError, {}, {}, "unknown-batch", {}, {}, {}});
			return result;
		}
		if (std::ranges::any_of(batches_, [&](const auto& item) {
			return item.second.parent && *item.second.parent == id;
		})) {
			EraseBatch(id);
			result.events.push_back({EventType::ProtocolError, {}, {}, "batch-close-order", {}, {}, {}});
			return result;
		}
		result.messages = FinishBatch(id, &result.events);
		if (!result.messages.empty()) result.consumed = false;
	} else {
		result.events.push_back({EventType::ProtocolError, {}, {}, "invalid-batch-reference", {}, {}, {}});
	}
	return result;
}

bool Engine::IsEcho(const Message& message)
{
	if (!IsEnabled("echo-message") || (message.command != "PRIVMSG" && message.command != "NOTICE") ||
		message.params.size() < 2 || !message.prefix || !SameIdentifier(NickFromPrefix(*message.prefix), nick_))
		return false;
	const auto* label = message.FindTag("label");
	for (auto it = pending_echoes_.begin(); it != pending_echoes_.end(); ++it) {
		const bool label_match = label && label->value && !it->label.empty() && *label->value == it->label;
		const bool content_match = it->command == message.command && SameIdentifier(it->target, message.params[0]) &&
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
	auto reject = [&](std::string key) {
		result.consumed = true;
		Event event;
		event.type = EventType::ProtocolError;
		event.source = source;
		event.key = std::move(key);
		result.events.push_back(std::move(event));
	};
	if (message.command == "ACCOUNT" && !message.params.empty()) {
		const std::string value = message.params[0] == "*" ? std::string{} : message.params[0];
		if (value.empty()) accounts_.erase(Casefold(source));
		else if (!BoundedAssign(&accounts_, Casefold(source), value, kMaxStateEntries)) {
			reject("account-state-limit");
			return result;
		}
		auto& event = emit(EventType::Account);
		event.value = value;
	} else if (message.command == "AWAY") {
		const std::string value = message.params.empty() ? std::string{} : message.params.back();
		if (value.empty()) away_.erase(Casefold(source));
		else if (!BoundedAssign(&away_, Casefold(source), value, kMaxStateEntries)) {
			reject("away-state-limit");
			return result;
		}
		auto& event = emit(EventType::Away);
		event.value = value;
	} else if (message.command == "CHGHOST" && message.params.size() >= 2) {
		const std::string value = message.params[0] + '@' + message.params[1];
		if (!BoundedAssign(&hosts_, Casefold(source), value, kMaxStateEntries)) {
			reject("host-state-limit");
			return result;
		}
		auto& event = emit(EventType::HostChanged);
		event.key = message.params[0];
		event.value = message.params[1];
	} else if (message.command == "SETNAME" && !message.params.empty()) {
		if (!BoundedAssign(&realnames_, Casefold(source), message.params.back(), kMaxStateEntries)) {
			reject("realname-state-limit");
			return result;
		}
		auto& event = emit(EventType::RealnameChanged);
		event.value = message.params.back();
	} else if (message.command == "RENAME") {
		// The server-to-client draft command always carries old channel, new
		// channel, and a (possibly empty) reason. Reject malformed input before it
		// can become a legacy unknown command or mutate channel-keyed state.
		if (message.params.size() != 3 || !ValidChannelName(message.params[0]) ||
			!ValidChannelName(message.params[1]) ||
			message.params[2].size() > kMaxMessageBytes || HasLineControl(message.params[2])) {
			reject("channel-rename-invalid");
			return result;
		}
		const auto old_name = Casefold(message.params[0]);
		const auto new_name = Casefold(message.params[1]);
		if (old_name != new_name && joined_channels_.contains(old_name) &&
			!BoundedInsert(&joined_channels_, new_name, kMaxStateEntries)) {
			reject("joined-state-limit");
			return result;
		}
		auto& event = emit(EventType::ChannelRenamed);
		event.target = message.params[0];
		event.value = message.params[1];
		if (message.params.size() > 2) event.detail = message.params[2];
		if (old_name != new_name) {
			joined_channels_.erase(old_name);
			const auto marker = read_markers_.find(old_name);
			if (marker != read_markers_.end()) {
				auto value = std::move(marker->second);
				read_markers_.erase(marker);
				read_markers_[new_name] = std::move(value);
			}
			const auto old_metadata = metadata_.find(old_name);
			if (old_metadata != metadata_.end()) {
				auto entries = std::move(old_metadata->second);
				metadata_.erase(old_metadata);
				if (const auto existing = metadata_.find(new_name); existing != metadata_.end()) {
					metadata_entries_ -= (std::min)(metadata_entries_, existing->second.size());
					metadata_.erase(existing);
				}
				metadata_[new_name] = std::move(entries);
			}
		}
	} else if (message.command == "MARKREAD" && message.params.size() >= 2) {
		if (!BoundedAssign(&read_markers_, Casefold(message.params[0]), message.params[1], kMaxStateEntries)) {
			reject("read-marker-state-limit");
			return result;
		}
		auto& event = emit(EventType::ReadMarker);
		event.target = message.params[0];
		event.value = message.params[1];
	} else if (message.command == "METADATA" && message.params.size() >= 4) {
		const std::string target = Casefold(message.params[0]);
		const std::string& key = message.params[1];
		const std::string& value = message.params.back();
		if (target.empty() || target.size() > kMaxIdentityBytes || !ValidMetadataKey(key) ||
			value.size() > kMaxMessageBytes) {
			reject("metadata-invalid");
			return result;
		}
		auto target_found = metadata_.find(target);
		if (target_found == metadata_.end()) {
			if (metadata_.size() >= kMaxMetadataTargets || metadata_entries_ >= kMaxMetadataEntries) {
				reject("metadata-state-limit");
				return result;
			}
			target_found = metadata_.emplace(target, std::map<std::string, std::string>{}).first;
		}
		auto& entries = target_found->second;
		const bool is_new = !entries.contains(key);
		if (is_new && (entries.size() >= kMaxMetadataPerTarget || metadata_entries_ >= kMaxMetadataEntries)) {
			reject("metadata-state-limit");
			return result;
		}
		entries[key] = value;
		if (is_new) ++metadata_entries_;
		auto& event = emit(EventType::Metadata);
		event.target = message.params[0];
		event.key = key;
		event.value = value;
		event.context.push_back("visibility=" + message.params[2]);
	} else if (message.command == "REDACT" && message.params.size() >= 2) {
		if (!BoundedInsert(&redacted_, message.params[1], kMaxStateEntries)) {
			reject("redaction-state-limit");
			return result;
		}
		auto& event = emit(EventType::Redaction);
		event.target = message.params[0];
		event.key = message.params[1];
		if (message.params.size() > 2) event.detail = message.params[2];
	} else if ((message.command == "REGISTER" || message.command == "VERIFY") &&
		message.params.size() >= 3) {
		auto& event = emit(EventType::AccountRegistration);
		event.key = message.params[0];
		event.target = message.params[1];
		event.value = message.command;
		event.detail = message.params.back();
	} else if ((message.command == "FAIL" || message.command == "WARN" || message.command == "NOTE") &&
		message.params.size() >= 3) {
		const bool registration = message.params[0] == "REGISTER" || message.params[0] == "VERIFY";
		auto& event = emit(registration ? EventType::AccountRegistration : EventType::StandardReply);
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
	if (!parsed) {
		invalid.events.push_back({EventType::ProtocolError, {}, {}, "invalid-line", {},
			parsed.error().reason, {}});
		return invalid;
	}
	Message message = std::move(*parsed);
	if (message.command == "PONG") {
		ProcessResult pong;
		pong.consumed = true;
		if (!message.params.empty() && message.params.back() == keepalive_token_)
			keepalive_token_.clear();
		return pong;
	}
	if (message.command == "PING") {
		ProcessResult pong;
		pong.consumed = true;
		Message response;
		response.command = "PONG";
		response.params = message.params;
		pong.outbound.push_back(response.Serialize(false));
		return pong;
	}
	if (const auto suppressed = flood_->Admit(message)) {
		ProcessResult abuse;
		abuse.consumed = true;
		Event event;
		event.type = EventType::Abuse;
		event.source = message.prefix ? NickFromPrefix(*message.prefix) : std::string{};
		event.key = *suppressed == FloodController::Kind::Line ? "line" :
			*suppressed == FloodController::Kind::Event ? "event" :
			*suppressed == FloodController::Kind::Ctcp ? "ctcp" :
			*suppressed == FloodController::Kind::Dcc ? "dcc" : "sound";
		event.value = "1000";
		event.detail = "suppressed";
		abuse.events.push_back(std::move(event));
		return abuse;
	}
	if (message.command == "CAP") return HandleCap(message);
	if (message.command == "AUTHENTICATE") return HandleAuthenticate(message);
	if (message.command == "BATCH") return HandleBatch(message);
	if (const auto* batch = message.FindTag("batch")) {
		if (!batch->value || batch->value->empty()) {
			ProcessResult result;
			result.consumed = true;
			result.events.push_back({EventType::ProtocolError, {}, {}, "invalid-batch-reference", {}, {}, {}});
			return result;
		}
		const auto found = batches_.find(*batch->value);
		if (found == batches_.end()) {
			ProcessResult result;
			result.consumed = true;
			result.events.push_back({EventType::ProtocolError, {}, {}, "unknown-batch", {}, {}, {}});
			return result;
		}
		const bool global_overflow = wire.size() > kMaxTotalBatchWireBytes - total_batch_wire_bytes_;
		if (found->second.messages.size() >= kMaxBatchMessages ||
			wire.size() > kMaxBatchWireBytes - found->second.wire_bytes || global_overflow) {
			EraseBatch(*batch->value);
			ProcessResult result;
			result.consumed = true;
			result.events.push_back({EventType::ProtocolError, {}, {},
				global_overflow ? "batch-global-limit" : "batch-limit", {}, {}, {}});
			return result;
		}
		found->second.wire_bytes += wire.size();
		total_batch_wire_bytes_ += wire.size();
		found->second.messages.push_back(std::move(message));
		ProcessResult result;
		result.consumed = true;
		return result;
	}
	if (message.command == "ACK") {
		// Labeled-response defines ACK as a server-only terminal response with
		// no parameters. Consume it before the historical command table so a
		// successful no-output command never becomes an "unknown command" line.
		ProcessResult acknowledgment;
		acknowledgment.consumed = true;
		const auto* label = message.FindTag("label");
		if (!IsEnabled("labeled-response") || !message.params.empty() || !label ||
			!label->value || label->value->empty()) {
			acknowledgment.events.push_back(
				{EventType::ProtocolError, {}, {}, "invalid-labeled-ack", {}, {}, {}});
			return acknowledgment;
		}
		Event event;
		event.type = EventType::LabeledResponse;
		event.key = *label->value;
		event.detail = "ack";
		const auto pending = label_commands_.find(event.key);
		if (pending != label_commands_.end()) {
			event.value = pending->second;
			label_commands_.erase(pending);
		}
		acknowledgment.events.push_back(std::move(event));
		return acknowledgment;
	}
	if (const auto numeric = NumericCommand(message.command)) {
		if (*numeric == 421 && cap_negotiating_ &&
			std::ranges::any_of(message.params, [](const std::string& param) {
				return Upper(param) == "CAP";
			})) {
			FinishRegistrationWithoutCapabilities();
			ProcessResult unsupported;
			unsupported.consumed = true;
			return unsupported;
		}
		if (*numeric >= 900 && *numeric <= 908) return HandleSaslNumeric(message, *numeric);
		if (*numeric >= 730 && *numeric <= 734) return HandleMonitorNumeric(message, *numeric);
		if (*numeric == 760 || *numeric == 761 || *numeric == 766 ||
			(*numeric >= 770 && *numeric <= 772) || *numeric == 774)
			return HandleMetadataNumeric(message, *numeric);
	}
	ProcessResult result;
	if (message.command == "004" && message.params.size() >= 3 &&
		message.params[1].size() <= kMaxIdentityBytes &&
		message.params[2].size() <= kMaxIdentityBytes) {
		server_identity_.server_name = message.params[1];
		server_identity_.version = message.params[2];
		server_identity_.profile = IdentifyServerProfile(server_identity_.version);
		Event event;
		event.type = EventType::ServerIdentity;
		event.source = server_identity_.server_name;
		event.value = server_identity_.version;
		event.key = std::to_string(static_cast<unsigned int>(server_identity_.profile));
		result.events.push_back(std::move(event));
	}
	if (message.command == "005") HandleIsupport(message, &result.events);
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
	AppendTagEvents(message, &result.events);
	if (message.prefix) {
		const std::string old_name = NickFromPrefix(*message.prefix);
		const std::string old_key = Casefold(old_name);
		auto erase_identity = [&](const std::string& key) {
			accounts_.erase(key);
			away_.erase(key);
			hosts_.erase(key);
			realnames_.erase(key);
			read_markers_.erase(key);
			monitor_online_.erase(key);
			monitor_list_.erase(key);
			const auto metadata = metadata_.find(key);
			if (metadata != metadata_.end()) {
				metadata_entries_ -= (std::min)(metadata_entries_, metadata->second.size());
				metadata_.erase(metadata);
			}
		};
		if (message.command == "NICK" && !message.params.empty()) {
			const bool self = SameIdentifier(old_name, nick_);
			const std::string new_key = Casefold(message.params[0]);
			auto move_identity = [&](auto* values) {
				const auto found = values->find(old_key);
				if (found == values->end() || old_key == new_key) return;
				auto value = std::move(found->second);
				values->erase(found);
				(*values)[new_key] = std::move(value);
			};
			move_identity(&accounts_);
			move_identity(&away_);
			move_identity(&hosts_);
			move_identity(&realnames_);
			move_identity(&read_markers_);
			move_identity(&monitor_online_);
			if (old_key != new_key) {
				const auto metadata = metadata_.find(old_key);
				if (metadata != metadata_.end()) {
					auto entries = std::move(metadata->second);
					metadata_.erase(metadata);
					const auto destination = metadata_.find(new_key);
					if (destination != metadata_.end()) {
						metadata_entries_ -= (std::min)(metadata_entries_, destination->second.size());
						metadata_.erase(destination);
					}
					metadata_[new_key] = std::move(entries);
				}
			}
			if (monitor_list_.erase(old_key)) (void)BoundedInsert(&monitor_list_, new_key, kMaxStateEntries);
			if (self) nick_ = message.params[0];
		} else if (message.command == "QUIT") {
			erase_identity(old_key);
		}
	}
	if (message.prefix && SameIdentifier(NickFromPrefix(*message.prefix), nick_)) {
		if (message.command == "JOIN" && !message.params.empty())
			(void)BoundedInsert(&joined_channels_, Casefold(message.params[0]), kMaxStateEntries);
		else if (message.command == "PART" && !message.params.empty())
			joined_channels_.erase(Casefold(message.params[0]));
	}
	if (message.command == "KICK" && message.params.size() >= 2 && SameIdentifier(message.params[1], nick_))
		joined_channels_.erase(Casefold(message.params[0]));
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
	ScopedSensitiveMessageWipe wipe_message(&message);
	if (!std::ranges::all_of(message.tags, [](const Tag& tag) { return ValidTagKey(tag.name); }))
		return std::unexpected(ParseFailure{"invalid outbound tag key"});
	if (isupport_.contains("UTF8ONLY")) {
		const bool valid_params = std::ranges::all_of(message.params, ValidUtf8);
		const bool valid_tags = std::ranges::all_of(message.tags, [](const Tag& tag) {
			return !tag.value || ValidUtf8(*tag.value);
		});
		if (!valid_params || !valid_tags)
			return std::unexpected(ParseFailure{"UTF8ONLY server rejects non-UTF-8 output"});
	}
	for (const auto& tag : message.tags) {
		if (!tag.name.starts_with('+')) continue;
		if (!IsEnabled("message-tags") || !ClientTagAllowed(tag.name))
			return std::unexpected(ParseFailure{"client-only tag is unavailable"});
	}
	if (!message.params.empty()) {
		if (const auto target_limit = TargetLimit(message.command)) {
			const auto targets = Split(message.params.front(), ',');
			if (targets.size() > *target_limit)
				return std::unexpected(ParseFailure{"command exceeds advertised target limit"});
		}
	}
	const bool protocol_control = message.command == "CAP" || message.command == "AUTHENTICATE" ||
		message.command == "PASS" || message.command == "NICK" || message.command == "USER" ||
		message.command == "PING" || message.command == "PONG";
	std::string label;
	if (!protocol_control && IsEnabled("labeled-response") && !message.FindTag("label")) {
		std::ostringstream stream;
		stream << "cc" << std::hex << next_label_++;
		label = stream.str();
		message.SetTag("label", label);
	}
	auto serialized = message.SerializeChecked(true);
	if (!serialized) return std::unexpected(serialized.error());
	if (!label.empty()) {
		if (label_commands_.size() >= kMaxPendingLabels) label_commands_.erase(label_commands_.begin());
		label_commands_[label] = message.command;
	}
	if (IsEnabled("echo-message") && (message.command == "PRIVMSG" || message.command == "NOTICE") &&
		message.params.size() >= 2) {
		pending_echoes_.push_back({label, message.command, message.params[0], message.params.back()});
		if (pending_echoes_.size() > 128) pending_echoes_.erase(pending_echoes_.begin());
	}
	if (message.command == "JOIN" && !message.params.empty()) {
		for (const auto& channel : Split(message.params[0], ','))
			(void)BoundedInsert(&joined_channels_, Casefold(channel), kMaxStateEntries);
	} else if (message.command == "PART" && !message.params.empty()) {
		for (const auto& channel : Split(message.params[0], ',')) joined_channels_.erase(Casefold(channel));
	}
	return serialized;
}

std::expected<std::string, ParseFailure> Engine::PrepareAccountRegistration(
	std::string account, std::string email, std::string&& password)
{
	ScopedStringWipe wipe(&password);
	if (!secure_transport_ || !IsEnabled("draft/account-registration"))
		return std::unexpected(ParseFailure{"account registration requires TLS and negotiated capability"});
	if (!ValidMiddleParameter(account) || !ValidMiddleParameter(email) ||
		password.empty() || password.size() > 4096 || HasLineControl(password))
		return std::unexpected(ParseFailure{"invalid account registration fields"});

	std::size_t minimum = 1;
	std::size_t maximum = 300;
	if (const auto value = CapabilityValue("draft/account-registration")) {
		for (const auto& token : Split(*value, ',')) {
			const auto equal = token.find('=');
			if (equal == std::string::npos) continue;
			const auto key = token.substr(0, equal);
			if (key != "min-password-length" && key != "max-password-length") continue;
			char* end = nullptr;
			const auto parsed = std::strtoull(token.c_str() + equal + 1, &end, 10);
			if (!end || *end || parsed == 0 || parsed > kMaxCredentialBytes) continue;
			if (key == "min-password-length") minimum = static_cast<std::size_t>(parsed);
			else maximum = static_cast<std::size_t>(parsed);
		}
	}
	if (minimum > maximum || password.size() < minimum || password.size() > maximum ||
		(isupport_.contains("UTF8ONLY") && !ValidUtf8(password)))
		return std::unexpected(ParseFailure{"password violates advertised account registration limits"});
	Message command;
	command.command = "REGISTER";
	command.params = {std::move(account), std::move(email), password};
	auto serialized = command.SerializeChecked(false);
	SecureClear(&command.params.back());
	return serialized;
}

std::expected<std::string, ParseFailure> Engine::PrepareAccountVerification(
	std::string account, std::string&& verification_code)
{
	ScopedStringWipe wipe(&verification_code);
	if (!secure_transport_ || !IsEnabled("draft/account-registration") ||
		!ValidMiddleParameter(account) || !ValidMiddleParameter(verification_code))
		return std::unexpected(ParseFailure{"invalid account verification request"});
	Message command;
	command.command = "VERIFY";
	command.params = {std::move(account), verification_code};
	auto serialized = command.SerializeChecked(false);
	SecureClear(&command.params.back());
	return serialized;
}

std::string Engine::PrepareOutgoing(std::string_view wire)
{
	auto prepared = PrepareOutgoingChecked(wire);
	return prepared ? std::move(*prepared) : std::string{};
}

} // namespace comic_chat::ircv3
