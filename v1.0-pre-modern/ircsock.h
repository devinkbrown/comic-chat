// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#ifndef COMIC_CHAT_V1_IRC_SOCKET_H
#define COMIC_CHAT_V1_IRC_SOCKET_H

#include "comicchat/net/connection_engine.hpp"
#include "comicchat/net/ircv3.hpp"
#include "transportadapter.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

inline constexpr UINT WM_COMICCHAT_V1_NETWORK_EVENT = WM_APP + 0x17E;

// Compatibility facade for the original Microsoft call shape. This class is
// deliberately not an MFC socket: ConnectionEngine exclusively owns DNS, TCP,
// timers, reconnects, and mbedTLS. All methods are called on the UI thread.
class CIrcSocket final {
public:
	CIrcSocket();
	~CIrcSocket();
	CIrcSocket(const CIrcSocket&) = delete;
	CIrcSocket& operator=(const CIrcSocket&) = delete;

	BOOL Connect(LPCSTR server, UINT port, BOOL secure_transport);
	void Close() noexcept;
	BOOL IsOpen() const noexcept;
	int Send(void* data, std::size_t byte_count);
	void PollNetworkEvents(LPARAM wakeup_cookie);

	void OnConnect(int error_code);
	void OnClose(int error_code);
	void ProcessMessage(char* line);

private:
	struct WakeupState final {
		std::atomic<HWND> hwnd{NULL};
		comic_chat::v1::transport::WakeupGate gate;
	};

	std::expected<comicchat::net::GenerationId,
		comic_chat::v1::transport::AdapterError> StartConnection(
			LPCSTR server, UINT port, BOOL secure_transport);
	std::expected<comicchat::net::SendId,
		comic_chat::v1::transport::AdapterError> QueueProtocolLine(std::string_view wire);
	void DispatchProtocolMessage(const comic_chat::ircv3::Message& message);
	void ProcessReceivedBytes(const comicchat::net::BytesReceived& received);
	void RequestUiWakeup(std::uint64_t cookie);
	void DrainNetworkEvents(std::uint64_t cookie);

	comicchat::net::ConnectionEngine connection_;
	std::shared_ptr<WakeupState> wakeup_state_;
	comic_chat::ircv3::Engine ircv3_;
	comic_chat::ircv3::LineFramer line_framer_;
	comic_chat::v1::transport::SessionGate session_;
	comicchat::net::GenerationId generation_{};
	comicchat::net::SendId next_send_id_{1};
	comicchat::net::State transport_state_{comicchat::net::State::stopped};
	std::string server_host_;
	std::string local_address_;
	BOOL transport_open_{FALSE};
	BOOL secure_transport_{FALSE};
};

// MainFrame owns the Windows message handler; the legacy socket object remains
// private to irc.cpp just as in the Microsoft source.
void ChatPollNetworkEvents(LPARAM wakeup_cookie);

#endif // COMIC_CHAT_V1_IRC_SOCKET_H
