#ifndef COMIC_CHAT_TRANSPORT_UI_BRIDGE_H
#define COMIC_CHAT_TRANSPORT_UI_BRIDGE_H

#include "../portable/include/comicchat/net/connection_engine.hpp"
#include "modernui.h"

namespace comic_chat::modern_ui {

constexpr TransportState TransportStateFor(::comicchat::net::State state)
{
	switch (state) {
	case ::comicchat::net::State::resolving:
	case ::comicchat::net::State::connecting:
	case ::comicchat::net::State::proxy_handshake:
	case ::comicchat::net::State::tls_handshake:
		return TransportState::connecting;
	case ::comicchat::net::State::reconnect_wait:
		return TransportState::reconnecting;
	case ::comicchat::net::State::connected:
		return TransportState::online;
	case ::comicchat::net::State::stopped:
	default:
		return TransportState::offline;
	}
}

} // namespace comic_chat::modern_ui

#endif
