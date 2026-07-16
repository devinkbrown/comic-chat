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

struct Message {
	std::vector<Tag> tags;
	std::optional<std::string> prefix;
	std::string command;
	std::vector<std::string> params;

	static std::expected<Message, ParseFailure> Parse(std::string_view wire);
	static bool Parse(std::string_view wire, Message* out, std::string* error = nullptr);
	std::string Serialize(bool include_tags = true) const;
	const Tag* FindTag(std::string_view name) const;
	Tag* FindTag(std::string_view name);
	void SetTag(std::string name, std::optional<std::string> value);
	void RemoveTag(std::string_view name);
};

enum class EventType {
	Account,
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
	bool allow_external = true;
	// Tests may pin a nonce. Production leaves this empty and uses the OS-backed
	// C++ random source before hashing the result into an IRC-safe token.
	std::string nonce;
};

struct StsPolicy {
	std::optional<unsigned int> port;
	std::optional<std::uint64_t> duration;
	bool preload = false;
};

struct ProcessResult {
	ProcessResult() = default;
	~ProcessResult();
	ProcessResult(ProcessResult&&) noexcept = default;
	ProcessResult& operator=(ProcessResult&&) noexcept = default;
	ProcessResult(const ProcessResult&) = delete;
	ProcessResult& operator=(const ProcessResult&) = delete;

	bool consumed = false;
	std::vector<Message> messages;
	std::vector<std::string> outbound;
	std::vector<Event> events;
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

	std::vector<std::string> BeginRegistration(
		const SaslConfig& sasl,
		std::string nick,
		bool secure_transport);
	ProcessResult Process(std::string_view wire);

	// Adds a unique label when labeled-response is active and remembers local
	// PRIVMSG/NOTICE messages so echo-message cannot create a second balloon.
	std::expected<std::string, ParseFailure> PrepareOutgoingChecked(std::string_view wire);
	std::string PrepareOutgoing(std::string_view wire);

	bool IsOffered(std::string_view capability) const;
	bool IsEnabled(std::string_view capability) const;
	std::optional<std::string> CapabilityValue(std::string_view capability) const;
	const std::map<std::string, std::string>& Accounts() const { return accounts_; }
	const std::map<std::string, std::string>& AwayMessages() const { return away_; }
	const std::map<std::string, std::string>& Hosts() const { return hosts_; }
	const std::map<std::string, std::string>& Realnames() const { return realnames_; }
	const std::map<std::string, std::string>& ReadMarkers() const { return read_markers_; }
	const std::map<std::string, std::map<std::string, std::string>>& Metadata() const { return metadata_; }
	const std::set<std::string>& RedactedMessageIds() const { return redacted_; }
	const std::optional<StsPolicy>& CurrentStsPolicy() const { return sts_policy_; }
	bool RegistrationFinished() const { return cap_end_sent_; }
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

	ProcessResult HandleCap(const Message& message);
	ProcessResult HandleAuthenticate(const Message& message);
	ProcessResult HandleSaslNumeric(const Message& message, unsigned int numeric);
	ProcessResult HandleBatch(const Message& message);
	ProcessResult HandleStateMessage(const Message& message);
	void ParseCapabilityList(std::string_view list, bool replace);
	std::vector<std::string> SelectCapabilities() const;
	std::vector<std::string> RequestCapabilities(const std::vector<std::string>& names);
	void MaybeFinishRegistration(std::vector<std::string>* outbound);
	void RemoveCapabilityAndDependents(std::string_view name);
	bool DependenciesAvailable(std::string_view name) const;
	bool DependenciesEnabled(std::string_view name) const;
	std::vector<Message> FinishBatch(const std::string& id, std::vector<Event>* events);
	bool IsEcho(const Message& message);
	void UpdateSts(std::optional<std::string> value, std::vector<Event>* events);

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
	std::set<std::string> redacted_;
	std::set<std::string> joined_channels_;
	std::optional<StsPolicy> sts_policy_;
	SaslConfig sasl_config_;
	std::unique_ptr<SaslSession> sasl_;
	std::string nick_;
	std::string ls_accumulator_;
	std::uint64_t next_label_ = 1;
	bool secure_transport_ = false;
	bool cap_negotiating_ = false;
	bool cap_end_sent_ = false;
	bool sasl_requested_ = false;
	bool sasl_terminal_ = false;
	bool sasl_succeeded_ = false;
};

} // namespace comic_chat::ircv3

#endif
