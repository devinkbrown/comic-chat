---
name: comicchat-ircv3-compat
description: Implement, review, and test IRC/IRCv3 parsing, capability negotiation, SASL-facing protocol state, tags, batches, typed events, server profiles, fixtures, and legacy MFC adaptation. Use when changing portable/src/net/ircv3.cpp, its public header/tests/fixtures, ircv3eventbridge.h, transportuibridge.h, legacy IRC consumers, or any advertised IRCv3 behavior.
---

# Comic Chat IRCv3 compatibility

## Establish the protocol oracle

1. Read AGENTS.md and portable/README.md.
2. Read references/spec-map.md, docs/IRCv3-COVERAGE.md, and the exact specification sections governing the change.
3. Recheck the live IRCv3 registry category and status. The registry omits `draft/` in its display, so preserve the normative wire identifier from the individual specification.
4. Trace the behavior from wire input through LineFramer and Engine, typed ProcessResult, the modern Windows bridge, and the legacy model/UI consumer.
5. Compare the relevant historical behavior in v2.5-beta-1/ without editing that snapshot.
6. Add a fixture or causal state-machine test before implementation.

Use comicchat-transport-security alongside this skill for TLS, proxy, secret lifetime, worker ownership, or DCC transport changes.

## Preserve parser and state contracts

- Treat command case according to IRC rules, but treat capability names, message-tag keys, batch types, labels, message IDs, and other specification-defined opaque identifiers with their exact required case semantics.
- Reject CR, LF, NUL, malformed parameters, invalid UTF-8 where required, and outbound injection before serialization.
- Preserve unknown well-formed tags and events for forward compatibility. Do not display TAGMSG as chat history by default.
- Keep borrowed views inside their source lifetime. Store owned bounded values for state that survives the current frame.
- Keep all maps, sets, batches, labels, metadata, monitor state, credentials, and accumulated wire bytes explicitly bounded.
- Make malformed or oversized batch state fail locally without poisoning later batches or registration.

## Preserve capability negotiation

- Start registration with CAP LS 302 and keep cap-notify's implicit semantics.
- Accumulate multiline LS replies before selecting capabilities.
- Keep offered, requested, pending, acknowledged, and enabled states distinct.
- Apply one CAP REQ atomically. Do not partially enable a multi-line ACK set or a partially rejected request.
- Request a capability only when its normative dependencies are available. Do not invent dependencies from convenient implementation order.
- Gate automatic requests on product readiness as well as parser recognition. A typed event without a legacy model/UI consumer is observe-only, not safe advertised support.
- Remove dependent capabilities when CAP DEL invalidates their prerequisites.
- Observe STS; never send it in CAP REQ. Apply an insecure upgrade or secure persistence only through the transport policy boundary.
- Finish with CAP END only after capability and SASL terminal state permits it; retain bounded fallback for servers that reject or ignore CAP.

## Adapt negotiated behavior

- Produce typed events and normalized state under portable/include/comicchat/net/ircv3.hpp. Keep old MFC message routing out of the portable protocol engine.
- Adapt typed events in v2.5-beta-1-modern/ircv3eventbridge.h and connection ownership in transportuibridge.h.
- Preserve readable ordinary IRC behavior and the historical Comic Chat model. Do not expose raw IRCv3 shapes directly to assumptions in ircproto.cpp, protsupp.cpp, histent.cpp, memblst.cpp, or chatdoc.cpp.
- Prevent echo-message from creating duplicate balloons while still completing label correlation.
- Reconnect only with explicitly safe idempotent recovery such as JOIN and bounded history requests. Never replay uncertain PRIVMSG or credentials.
- Keep server-profile exceptions narrow, fixture-backed, and named. Do not weaken the generic parser to accommodate one daemon.

## Build realistic fixtures

- Store server transcript fixtures under portable/tests/fixtures/irc/.
- Cover Solanum, UnrealIRCd, ircu, Orochi, and InspIRCd shapes when the behavior differs.
- Preserve real prefixes, numerics, capability values, tags, continuation markers, and ordering relevant to the contract.
- Add the smallest transcript needed to prove the vendor shape. Do not copy secrets, production user data, or a giant unrelated session.
- Test the generic rule separately from each necessary vendor exception.

## Verify end to end

Run the standalone protocol suite:

~~~sh
meson test -C <build-dir> comicchat-ircv3 --print-errorlogs
~~~

Then run the full headless suite and the MSVC consumer when shared headers, bridge behavior, or legacy-visible events change. Run ASan+UBSan for parsing, framing, batch, metadata, or credential changes.

Require direct tests for the changed contract and its negative cases. A parser-only assertion does not prove legacy adaptation; include bridge/model evidence when user-visible state changes. A test source is not a gate unless portable/meson.build, chat.mak, or .github/workflows/build-modern.yml actually builds or runs it.

Update docs/IRCv3-COVERAGE.md in the same change whenever capability selection,
an identifier's status, a parser/state/event/adapter path, a server fixture, or an
auto-request safety conclusion changes. Do not mark IRCv3 or an extension complete
without the evidence required by that ledger.

Report the official spec URL and section, fixture identity, baseline failure, exact tests, pass counts, ledger change, and any intentionally unsupported or draft behavior.
