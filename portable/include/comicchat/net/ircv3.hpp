#ifndef COMIC_CHAT_NET_IRCV3_HPP
#define COMIC_CHAT_NET_IRCV3_HPP

#if defined(_MSC_VER)
#if !defined(_MSVC_LANG) || _MSVC_LANG <= 202302L
#error "Comic Chat shared networking requires the post-C++23/C++26 language mode"
#endif
#elif __cplusplus <= 202302L
#error "Comic Chat shared networking requires the post-C++23/C++26 language mode"
#endif

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace comic_chat::ircv3 {

struct Tag {
	std::string name;
	std::optional<std::string> value;
};

struct ParseFailure {
	std::string reason;
};

enum class TypingStatus {
	Active,
	Paused,
	Done,
};

struct TypedTags {
	std::optional<std::string> reply;
	std::optional<std::string> reaction;
	std::optional<std::string> unreaction;
	std::optional<TypingStatus> typing;
	std::optional<std::string> channel_context;
	std::optional<std::string> message_id;
	std::optional<std::string> oper;
	bool bot = false;
};

struct Message {
	std::vector<Tag> tags;
	std::optional<std::string> prefix;
	std::string command;
	std::vector<std::string> params;

	static std::expected<Message, ParseFailure> Parse(std::string_view wire);
	static bool Parse(std::string_view wire, Message* out, std::string* error = nullptr);
	std::expected<std::string, ParseFailure> SerializeChecked(bool include_tags = true) const;
	std::string Serialize(bool include_tags = true) const;
	TypedTags DecodeTags() const;
	const Tag* FindTag(std::string_view name) const;
	Tag* FindTag(std::string_view name);
	void SetTag(std::string name, std::optional<std::string> value);
	void RemoveTag(std::string_view name);
};

enum class EventType {
	Account,
	AccountRegistration,
	Away,
	HostChanged,
	RealnameChanged,
	ChannelRenamed,
	ReadMarker,
	Metadata,
	Redaction,
	StandardReply,
	LabeledResponse,
	ChatHistory,
	Multiline,
	StsPolicy,
	MonitorOnline,
	MonitorOffline,
	MonitorList,
	MonitorListEnd,
	MonitorListFull,
	Isupport,
	ServerIdentity,
	Typing,
	Reaction,
	Netsplit,
	Netjoin,
	ProtocolError,
	MessageContext,
	Abuse,
};

struct Event {
	EventType type = EventType::StandardReply;
	std::string source;
	std::string target;
	std::string key;
	std::string value;
	std::string detail;
	std::vector<std::string> context;
};

struct SaslConfig {
	std::string authentication_id;
	std::string password;
	std::string authorization_id;
	// EXTERNAL requires a configured client certificate. Callers must opt in
	// after provisioning one rather than advertising it implicitly.
	bool allow_external = false;
	// Tests may pin a nonce. Production leaves this empty and uses the shared
	// OS-seeded PSA CSPRNG to produce an IRC-safe token.
	std::string nonce;
};

struct StsPolicy {
	std::optional<unsigned int> port;
	std::optional<std::uint64_t> duration;
	bool preload = false;
};

enum class StsPolicyAction {
	Upgrade,
	Persist,
	Remove,
};

// A typed, transport-facing result of processing an STS advertisement. Only a
// certificate-verified Engine emits Persist or Remove. Upgrade is emitted only
// on plaintext and must be applied before any output from the same IRC line.
struct StsPolicyUpdate {
	StsPolicyAction action = StsPolicyAction::Upgrade;
	std::uint16_t port{};
	std::uint64_t duration{};
	bool preload = false;
};

enum class CaseMapping {
	Ascii,
	Rfc1459,
	StrictRfc1459,
};

enum class ServerProfile {
	Unknown,
	Solanum,
	UnrealIRCd,
	Ircu,
	Orochi,
	InspIRCd,
};

struct ServerIdentity {
	ServerProfile profile = ServerProfile::Unknown;
	std::string server_name;
	std::string version;
};

struct FloodSnapshot {
	std::uint64_t accepted_lines = 0;
	std::uint64_t suppressed_lines = 0;
	std::uint64_t suppressed_events = 0;
	std::uint64_t suppressed_ctcp = 0;
	std::uint64_t suppressed_dcc = 0;
	std::uint64_t suppressed_sound = 0;
};

struct ProcessResult {
	ProcessResult() = default;
	~ProcessResult();
	ProcessResult(ProcessResult&& other) noexcept;
	ProcessResult& operator=(ProcessResult&& other) noexcept;
	ProcessResult(const ProcessResult&) = delete;
	ProcessResult& operator=(const ProcessResult&) = delete;

	bool consumed = false;
	std::vector<Message> messages;
	std::vector<std::string> outbound;
	std::vector<Event> events;
	std::optional<StsPolicyUpdate> sts_update;
};

class LineFramer {
public:
	explicit LineFramer(std::size_t maximum_line_bytes = 8191 + 512);
	std::expected<std::vector<std::string>, std::string> Push(std::span<const std::byte> bytes);
	bool Push(std::string_view bytes, std::vector<std::string>* lines, std::string* error = nullptr);
	void Reset();
	std::size_t BufferedBytes() const { return buffer_.size(); }

private:
	std::string buffer_;
	std::size_t maximum_line_bytes_;
};

class Engine {
public:
	Engine();
	~Engine();
	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;

	// Lvalues are copied and remain caller-owned. The rvalue overload consumes
	// and overwrites source storage so short-string credentials cannot survive
	// in moved-from SSO buffers.
	std::vector<std::string> BeginRegistration(
		const SaslConfig& sasl,
		std::string nick,
		bool secure_transport);
	std::vector<std::string> BeginRegistration(
		SaslConfig&& sasl,
		std::string nick,
		bool secure_transport);
	ProcessResult Process(std::string_view wire);
	// Completes registration without CAP END for a server that rejected or
	// ignored CAP. This is also the bounded timeout fallback used by legacy
	// ircu-family servers.
	void FinishRegistrationWithoutCapabilities();
	std::vector<std::string> FinishRegistrationAfterTimeout();
	std::expected<std::string, ParseFailure> PrepareKeepalivePing();
	bool KeepaliveOutstanding() const { return !keepalive_token_.empty(); }

	// Adds a unique label when labeled-response is active and remembers local
	// PRIVMSG/NOTICE messages so echo-message cannot create a second balloon.
	std::expected<std::string, ParseFailure> PrepareOutgoingChecked(std::string_view wire);
	std::string PrepareOutgoing(std::string_view wire);
	std::expected<std::string, ParseFailure> PrepareAccountRegistration(
		std::string account, std::string email, std::string&& password);
	std::expected<std::string, ParseFailure> PrepareAccountVerification(
		std::string account, std::string&& verification_code);

	bool IsOffered(std::string_view capability) const;
	bool IsEnabled(std::string_view capability) const;
	std::optional<std::string> CapabilityValue(std::string_view capability) const;
	const std::map<std::string, std::string>& Accounts() const { return accounts_; }
	const std::map<std::string, std::string>& AwayMessages() const { return away_; }
	const std::map<std::string, std::string>& Hosts() const { return hosts_; }
	const std::map<std::string, std::string>& Realnames() const { return realnames_; }
	const std::map<std::string, std::string>& ReadMarkers() const { return read_markers_; }
	const std::map<std::string, std::map<std::string, std::string>>& Metadata() const { return metadata_; }
	const std::set<std::string>& MetadataSubscriptions() const { return metadata_subscriptions_; }
	const std::set<std::string>& RedactedMessageIds() const { return redacted_; }
	const std::map<std::string, std::string>& Isupport() const { return isupport_; }
	const std::map<std::string, std::string>& MonitorOnline() const { return monitor_online_; }
	const std::set<std::string>& MonitorList() const { return monitor_list_; }
	std::optional<std::size_t> MonitorLimit() const { return monitor_limit_; }
	std::optional<std::size_t> TargetLimit(std::string_view command) const;
	bool ClientTagAllowed(std::string_view tag) const;
	std::optional<std::string> NetworkIconUrl() const;
	const ServerIdentity& Identity() const { return server_identity_; }
	const FloodSnapshot& FloodState() const;
	CaseMapping CurrentCaseMapping() const { return case_mapping_; }
	const std::optional<StsPolicy>& CurrentStsPolicy() const { return sts_policy_; }
	bool RegistrationFinished() const { return cap_end_sent_; }
	bool CapabilityResponseSeen() const { return cap_response_seen_; }
	bool SaslSucceeded() const { return sasl_succeeded_; }
	bool SecretsCleared() const;
	std::vector<std::string> RecoveryCommands(std::size_t history_limit = 100) const;

private:
	struct Batch {
		std::string type;
		std::vector<std::string> params;
		std::vector<Message> messages;
		std::optional<std::string> label;
		std::optional<std::string> parent;
		std::size_t wire_bytes = 0;
	};
	struct PendingEcho {
		std::string label;
		std::string command;
		std::string target;
		std::string text;
	};
	struct CapabilityRequest {
		std::set<std::string> names;
		std::set<std::string> acknowledged;
	};
	class SaslSession;
	class FloodController;

	ProcessResult HandleCap(const Message& message);
	ProcessResult HandleAuthenticate(const Message& message);
	ProcessResult HandleSaslNumeric(const Message& message, unsigned int numeric);
	ProcessResult HandleMonitorNumeric(const Message& message, unsigned int numeric);
	ProcessResult HandleMetadataNumeric(const Message& message, unsigned int numeric);
	ProcessResult HandleBatch(const Message& message);
	ProcessResult HandleStateMessage(const Message& message);
	void HandleIsupport(const Message& message, std::vector<Event>* events);
	void AppendTagEvents(const Message& message, std::vector<Event>* events) const;
	void ParseCapabilityList(std::string_view list, bool replace);
	std::vector<std::string> SelectCapabilities() const;
	std::vector<std::string> RequestCapabilities(const std::vector<std::string>& names);
	void MaybeFinishRegistration(std::vector<std::string>* outbound);
	void RemoveCapabilityAndDependents(std::string_view name);
	bool DependenciesAvailable(std::string_view name) const;
	bool DependenciesEnabled(std::string_view name) const;
	std::vector<Message> FinishBatch(const std::string& id, std::vector<Event>* events);
	void EraseBatch(const std::string& id);
	bool IsEcho(const Message& message);
	std::optional<StsPolicyUpdate> UpdateSts(
		std::optional<std::string> value, std::vector<Event>* events);
	std::string Casefold(std::string_view value) const;
	bool SameIdentifier(std::string_view left, std::string_view right) const;
	void ReindexState(CaseMapping previous);

	std::map<std::string, std::optional<std::string>> offered_;
	std::set<std::string> enabled_;
	std::set<std::string> pending_ack_;
	std::vector<CapabilityRequest> capability_requests_;
	std::map<std::string, Batch> batches_;
	std::vector<PendingEcho> pending_echoes_;
	std::map<std::string, std::string> label_commands_;
	std::map<std::string, std::string> accounts_;
	std::map<std::string, std::string> away_;
	std::map<std::string, std::string> hosts_;
	std::map<std::string, std::string> realnames_;
	std::map<std::string, std::string> read_markers_;
	std::map<std::string, std::map<std::string, std::string>> metadata_;
	std::set<std::string> metadata_subscriptions_;
	std::set<std::string> redacted_;
	std::set<std::string> joined_channels_;
	std::map<std::string, std::string> isupport_;
	std::map<std::string, std::string> monitor_online_;
	std::set<std::string> monitor_list_;
	std::optional<std::size_t> monitor_limit_;
	std::map<std::string, std::size_t> target_limits_;
	std::optional<std::size_t> max_targets_;
	ServerIdentity server_identity_;
	std::optional<StsPolicy> sts_policy_;
	SaslConfig sasl_config_;
	std::unique_ptr<SaslSession> sasl_;
	std::unique_ptr<FloodController> flood_;
	std::string nick_;
	std::string ls_accumulator_;
	std::size_t total_batch_wire_bytes_ = 0;
	std::size_t metadata_entries_ = 0;
	std::uint64_t next_label_ = 1;
	std::uint64_t next_keepalive_ = 1;
	std::string keepalive_token_;
	CaseMapping case_mapping_ = CaseMapping::Rfc1459;
	bool secure_transport_ = false;
	bool cap_negotiating_ = false;
	bool cap_end_sent_ = false;
	bool sasl_requested_ = false;
	bool sasl_terminal_ = false;
	bool sasl_succeeded_ = false;
	bool cap_response_seen_ = false;
};

} // namespace comic_chat::ircv3

#endif
