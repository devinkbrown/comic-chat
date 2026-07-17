#include "../transportuibridge.h"

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

int main() { return 0; }
