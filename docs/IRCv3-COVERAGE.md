# IRC and IRCv3 coverage

Audit snapshot: 2026-07-16, base source commit `0f42423`.

This is the compatibility ledger for the legacy Microsoft Comic Chat client in
`v2.5-beta-1-modern/` and the shared protocol engine in `portable/`. It measures
observable product behavior, not the number of identifiers recognized by the
parser. **Comic Chat: Reinked does not currently implement all of IRCv3.** The
portable engine has substantial, bounded protocol machinery, but many negotiated
features stop at retained state or a typed event that no legacy model or view
consumes.

Update this file whenever a capability, tag, batch type, ISUPPORT token, fixture,
or legacy IRCv3 adapter changes. A capability must not move to **implemented**
without causal parser/state tests and a legacy-consumer test where it changes
visible behavior.

## Status and evidence rules

| Status | Meaning in this ledger |
| --- | --- |
| **implemented** | The normative wire behavior, bounded state, product adaptation, and relevant negative cases have direct evidence. |
| **partial** | A useful path exists, but a direction, normative rule, recovery behavior, or product consumer is missing. |
| **observe-only** | Input is preserved, retained, or emitted as a typed event, but does not yet produce the required legacy-client behavior. |
| **unsupported** | There is no adequate implementation evidence, or the negotiated form is unsafe for the legacy consumer. |

“Tested” means the test is wired into the build, not merely present in the tree.
The current standalone gate is `meson test -C portable/build
comicchat-ircv3 --print-errorlogs`; it passed 1/1 with Clang 22.1.5 during this
audit. The five server files are curated transcript fixtures, not live-server
certification.

Primary protocol sources:

- [Modern IRC client protocol](https://modern.ircdocs.horse/)
- [IRCv3 specification index](https://ircv3.net/irc/)
- [IRCv3 registry](https://ircv3.net/registry)
- [Capability negotiation](https://ircv3.net/specs/extensions/capability-negotiation)
- [Message tags](https://ircv3.net/specs/extensions/message-tags)

The registry displays draft entries without the required `draft/` prefix. The
**wire identifier** column below retains the prefix required by each work-in-
progress specification. Categories matter: tags, batch types, commands, and
ISUPPORT tokens must never be sent in `CAP REQ` merely because they appear in a
transcript or registry.

## Product path audited

The intended path is:

1. `LineFramer` and `Message` parse bounded wire input
   (`portable/include/comicchat/net/ircv3.hpp:52-67,172-183`,
   `portable/src/net/ircv3.cpp:660-902`).
2. `Engine` negotiates capabilities and returns messages, outbound commands,
   and typed events (`portable/include/comicchat/net/ircv3.hpp:158-170,185-337`).
3. The Windows adapter feeds transport bytes to the engine, sends engine output,
   and strips tags only after queuing a complete `MessageContext`; `TAGMSG`
   becomes typed-only input and cannot enter the historical command table
   (`v2.5-beta-1-modern/ircv3eventbridge.h:24-51`,
   `v2.5-beta-1-modern/ircsock.cpp:736-747`).
4. The main frame drains bounded events and broadcasts
   `WM_COMICCHAT_IRCV3_EVENT`
   (`v2.5-beta-1-modern/ircv3eventbridge.h:13-78`,
   `v2.5-beta-1-modern/mainfrm.cpp:66-94`).
5. Legacy command/model code applies visible behavior.

Step 5 is the principal gap. There is no message-map handler for
`WM_COMICCHAT_IRCV3_EVENT`; repository-wide uses are only its declaration and
the broadcast. The legacy command table also has no `ACCOUNT`, `CHGHOST`,
`SETNAME`, `RENAME`, `FAIL`, `WARN`, or `NOTE`
(`v2.5-beta-1-modern/ircsock.cpp:118-172`). Therefore a typed portable event is
not, by itself, legacy-client support.

## Capability request safety

`CapabilityCatalog()` currently requests every recognized offered capability,
subject only to declared dependencies and the secure-SASL check
(`portable/src/net/ircv3.cpp:465-520,1327-1449`). STS is correctly observed and
never requested. `MONITOR`, client tags, batch types, and ISUPPORT tokens are
also correctly excluded (`portable/tests/ircv3_test.cpp:189-285`).

The default request set is too broad for the legacy product. “Auto-request” in
this table means what the current engine does, not what it should do after the
release gate is fixed.

Official capability sources: [account notify](https://ircv3.net/specs/extensions/account-notify),
[account registration](https://ircv3.net/specs/extensions/account-registration),
[account tag](https://ircv3.net/specs/extensions/account-tag),
[away notify](https://ircv3.net/specs/extensions/away-notify),
[batch](https://ircv3.net/specs/extensions/batch),
[channel rename](https://ircv3.net/specs/extensions/channel-rename),
[chathistory/event playback](https://ircv3.net/specs/extensions/chathistory),
[chghost](https://ircv3.net/specs/extensions/chghost),
[echo message](https://ircv3.net/specs/extensions/echo-message),
[extended ISUPPORT](https://ircv3.net/specs/extensions/extended-isupport),
[extended join](https://ircv3.net/specs/extensions/extended-join),
[extended monitor](https://ircv3.net/specs/extensions/extended-monitor),
[invite notify](https://ircv3.net/specs/extensions/invite-notify),
[labeled response](https://ircv3.net/specs/extensions/labeled-response),
[message redaction](https://ircv3.net/specs/extensions/message-redaction),
[metadata](https://ircv3.net/specs/extensions/metadata),
[MONITOR](https://ircv3.net/specs/extensions/monitor),
[multi-prefix](https://ircv3.net/specs/extensions/multi-prefix),
[multiline](https://ircv3.net/specs/extensions/multiline),
[no implicit NAMES](https://ircv3.net/specs/extensions/no-implicit-names),
[pre-away](https://ircv3.net/specs/extensions/pre-away),
[read marker](https://ircv3.net/specs/extensions/read-marker),
[SASL](https://ircv3.net/specs/extensions/sasl-3.2),
[server time](https://ircv3.net/specs/extensions/server-time),
[setname](https://ircv3.net/specs/extensions/setname),
[standard replies](https://ircv3.net/specs/extensions/standard-replies), and
[userhost in NAMES](https://ircv3.net/specs/extensions/userhost-in-names).

| Wire identifier | Spec status | Product status | Current evidence | Auto-request assessment |
| --- | --- | --- | --- | --- |
| `message-tags` | stable | **partial** | Bounded input preserves opaque keys while checked output remains strict (`portable/src/net/ircv3.cpp:676-768,2561-2568`); `TAGMSG` is typed-only at the legacy boundary (`v2.5-beta-1-modern/ircv3eventbridge.h:24-51`); causal tests `portable/tests/ircv3_test.cpp:142-181,632-647,1239-1264` | Safe to request at the transport boundary. Product status remains partial because typed contexts, typing, and reactions still lack visible descendant consumers. |
| `batch` | stable | **partial** | Bounded nesting and several batch families in `portable/src/net/ircv3.cpp:1989-2172`; tests `portable/tests/ircv3_test.cpp:313-348,744-787` | Allow only with audited member semantics; a generic batch can unwrap a command the legacy model cannot safely consume. |
| `server-time` | stable | **observe-only** | `time` is preserved as an unknown/generic tag and stripped after `MessageContext`; Solanum/Unreal/Orochi/InspIRCd fixtures | Safe for ordinary chat, but the legacy history timestamp is not set from the tag. |
| `standard-replies` | stable | **observe-only** | `FAIL`/`WARN`/`NOTE` become `StandardReply` events in `portable/src/net/ircv3.cpp:2193-2343`; state tests `portable/tests/ircv3_test.cpp:350-381` | **Block pending UI.** Consuming replies without a descendant event handler can hide errors from the user. |
| `cap-notify` | stable | **implemented** | Implicit under CAP 302; `NEW`/`DEL`, dependent removal, and request tracking in `portable/src/net/ircv3.cpp:1488-1615`; tests `portable/tests/ircv3_test.cpp:189-252` | Safe. Never remove the implicit state merely because a server sends `CAP DEL cap-notify`. |
| `account-notify` | stable | **observe-only** | Bounded account state and `Account` event in `portable/src/net/ircv3.cpp:2193-2343`; tests `portable/tests/ircv3_test.cpp:350-381,789-837` | Block until member identity/model adaptation exists. |
| `account-tag` | stable | **observe-only** | Tag is preserved in `MessageContext`; fixture coverage, but no typed field or legacy use | Safe to parse; do not claim account-aware display or moderation. Prefer request gating until a consumer exists. |
| `away-notify` | stable | **observe-only** | Bounded away state and `Away` event; state/bound tests | Block until member state is applied. |
| `bot` | non-standard vendor CAP | **observe-only** | Requested only when `message-tags` is also offered; `bot` typed flag; Orochi and Unreal fixtures | **Block or profile-gate.** The standard bot-mode contract is the `bot` tag plus `BOT` ISUPPORT, not a general `bot` capability. Current request is not actually restricted to Orochi. |
| `chghost` | stable | **observe-only** | Bounded host state and `HostChanged` event; state/bound tests | Block until identity/model adaptation exists. |
| `echo-message` | stable | **partial** | Outgoing labels/content retained and inbound echoes suppressed in `portable/src/net/ircv3.cpp:2174-2191,2539-2593`; tests `portable/tests/ircv3_test.cpp:383-396` | Usable, but content-only fallback can mistake an identical echo from another bouncer session for the local message. Prefer labels when available. |
| `extended-join` | stable | **unsupported** | Catalog and fixture selection only; no normalization before legacy `JOIN` | **Block.** Extended JOIN adds account and realname. The legacy handler treats `lastString` as the channel (`v2.5-beta-1-modern/ircsock.cpp:1374-1434`), so a realname can become the channel key. |
| `invite-notify` | stable | **unsupported** | Catalog presence; no causal extension test or adapter | **Block.** The legacy INVITE path assumes an invitation for the local user (`v2.5-beta-1-modern/ircsock.cpp:1361-1367`). |
| `labeled-response` | stable | **partial** | Labels, request correlation, labeled-response batch event, and echo correlation; `portable/src/net/ircv3.cpp:1989-2111,2539-2593`; tests `portable/tests/ircv3_test.cpp:383-396,744-787` | Block until no-response `ACK` and all response shapes are consumed instead of reaching legacy unknown-command handling. |
| `multi-prefix` | stable | **unsupported** | Negotiated but not normalized; no direct test | **Block.** `CUserInfo` strips only one prefix (`v2.5-beta-1-modern/userinfo.cpp:121-144`), so `@+nick` becomes nickname `+nick`. |
| `no-implicit-names` | stable | **unsupported** | Catalog selection; legacy self-JOIN creates only a response-tracking `CCQuery` (`v2.5-beta-1-modern/ircsock.cpp:1419-1434`, `v2.5-beta-1-modern/query.cpp:105-110`) | **Block.** It does not transmit `NAMES`; when the server suppresses the implicit reply, the member list can remain empty. |
| `sasl` | stable | **partial** | PLAIN, explicit EXTERNAL, and SCRAM-SHA-256; see dedicated section | Safe only on verified TLS with a usable mechanism. Product has no client-certificate provisioning and no post-registration reauthentication. |
| `setname` | stable | **observe-only** | `SETNAME` state and `RealnameChanged` event; state/bound tests | Block until the member model/view consumes it. |
| `userhost-in-names` | stable | **unsupported** | Negotiated but no legacy NAMES normalization or direct test | **Block.** NAMES tokens flow into the single-prefix `CUserInfo` constructor; `nick!user@host` can become the nickname. |
| `draft/channel-rename` | work in progress | **observe-only** | `RENAME` migrates portable channel-keyed state and emits `ChannelRenamed`; `portable/src/net/ircv3.cpp:2193-2343`; test `portable/tests/ircv3_test.cpp:873-883` | **Block.** No legacy document/tab/member-list rename occurs. |
| `draft/account-registration` | work in progress | **partial** | Secret-consuming `REGISTER`/`VERIFY` builders and typed outcomes in `portable/src/net/ircv3.cpp:2595-2658`; tests `portable/tests/ircv3_test.cpp:1083-1112` | Block until an account UI consumes the API and replies. |
| `draft/chathistory` | work in progress | **partial** | History batches and bounded recovery command builder in `portable/src/net/ircv3.cpp:1315-1325,1989-2111`; tests `portable/tests/ircv3_test.cpp:398-408,744-787` and four CAP fixtures | **Block for now.** No production caller uses `RecoveryCommands()`. Requesting chathistory can suppress server auto-playback without replacing it. |
| `draft/event-playback` | work in progress | **partial** | Dependency and generic batch unwrapping; no historical/live distinction at legacy boundary | **Block.** Historical JOIN/MODE/NICK/etc. can be replayed into the legacy model as live state. |
| `draft/extended-isupport` | work in progress | **partial** | `isupport`/`draft/isupport` batches and bounded generic 005 state in `portable/src/net/ircv3.cpp:1755-1837,1989-2111`; tests `portable/tests/ircv3_test.cpp:621-667,744-787` | Keep gated until batch replacement/removal semantics and legacy consumers are causal-tested. |
| `draft/metadata-2` | work in progress | **observe-only** | Bounded values/subscriptions, numerics, commands and batches in `portable/src/net/ircv3.cpp:1887-1949,1989-2111`; tests `portable/tests/ircv3_test.cpp:1044-1081` | Block until product consumers and outbound UI exist. |
| `draft/message-redaction` | work in progress | **observe-only** | Retained IDs and `Redaction` event; state/bound tests | **Block.** No handler removes or marks the rendered balloon, so advertised support leaves redacted content visible. |
| `draft/multiline` | work in progress | **partial** | Receive-side reassembly and `Multiline` event; batch tests | **Block.** No outbound builder; advertised `max-bytes`/`max-lines`, opening target, and prohibited blank/concat cases are not fully enforced. |
| `draft/oper-tag` | work in progress | **observe-only** | `draft/oper` and unprefixed `oper` decode to typed context; typed-tag tests | Block until the renderer consumes the status. Do not emit or prefer the unprefixed tag while the spec remains draft. |
| `draft/pre-away` | work in progress | **unsupported** | Catalog entry only; no pre-registration AWAY builder or causal test | **Block.** Negotiation without use supplies no product feature. |
| `draft/read-marker` | work in progress | **observe-only** | Bounded `MARKREAD` state and `ReadMarker` event; state/bound tests | Block until read state is displayed and outbound advancement is implemented. |
| `extended-monitor` | stable | **observe-only** | Presence numerics/state and typed events, generic outgoing path; `portable/src/net/ircv3.cpp:1755-1885`; tests `portable/tests/ircv3_test.cpp:621-667` | Block until the UI manages a MONITOR list. This CAP does not replace the base `MONITOR` ISUPPORT token. |
| `sts` | stable, server-advertised | **partial** | Observed outside the request catalog and immediate plaintext upgrade exists; see dedicated section | Correctly never request. Do not claim persistent STS until a durable per-host policy store exists. |

### Required request-policy change

Replace “request every recognized offered capability” with an explicit product-
readiness policy. At minimum, the unsafe rows above must default off until their
legacy adapters and causal tests land. Parsing must remain liberal and bounded
even when requesting is disabled. Server profiles must not silently enable a
feature unless a fixture and a profile-specific contract justify the exception.

## Non-capability registry coverage

### Message tags

[Message tags](https://ircv3.net/specs/extensions/message-tags) require clients
to treat keys as opaque, case-sensitive identifiers and not reject an otherwise
valid message solely because a tag key does not match a locally known grammar.
Input parsing now preserves those names and final duplicate values exactly while
retaining the 8,191-byte tag-frame and 510-byte payload bounds. The outbound
engine and serializer still enforce the current tag-key grammar and the smaller
client tag-frame bound (`portable/src/net/ircv3.cpp:676-768,2561-2568`).

| Wire tag | Spec | Status | Parser/state/event and product evidence |
| --- | --- | --- | --- |
| `account` | [account-tag](https://ircv3.net/specs/extensions/account-tag) | **observe-only** | Preserved generically; no typed field or member-account update. |
| `batch` | [batch](https://ircv3.net/specs/extensions/batch) | **partial** | Drives bounded batch membership and is consumed by `HandleBatch`; batch tests cover nesting and limits. |
| `bot` | [bot mode](https://ircv3.net/specs/extensions/bot-mode) | **observe-only** | Typed boolean exists; no renderer/member-model consumer. `BOT` is separately an ISUPPORT token. |
| `label` | [labeled response](https://ircv3.net/specs/extensions/labeled-response) | **partial** | Correlated to requests and opening batches; no full legacy response adapter. |
| `msgid` | [message IDs](https://ircv3.net/specs/extensions/message-ids) | **observe-only** | Decoded into `TypedTags::message_id`; only attached to typing/reaction context or generic `MessageContext`. |
| `time` | [server time](https://ircv3.net/specs/extensions/server-time) | **observe-only** | Preserved but not parsed or applied to a Comic Chat history entry. |
| `draft/multiline-concat` | [multiline](https://ircv3.net/specs/extensions/multiline) | **partial** | Used during receive-side reassembly; normative blank/concat constraints remain incomplete. |
| `+reply` | [reply](https://ircv3.net/specs/client-tags/reply) | **observe-only** | Decoded and outbound policy-checked; no reply UI or causal visible-product test. The Orochi fixture's old `+draft/reply` is only preserved generically. |
| `+draft/react`, `+draft/unreact` | [react](https://ircv3.net/specs/client-tags/react) | **observe-only** | Both draft and premature unprefixed spellings decode and emit `Reaction`; no visible consumer. Emit only draft-prefixed names while required by the WIP spec. |
| `+typing` | [typing](https://ircv3.net/specs/client-tags/typing) | **observe-only** | `active`/`paused`/`done` emit `Typing`; no typing-indicator consumer. |
| `+channel-context` | [channel context](https://ircv3.net/specs/client-tags/channel-context) | **observe-only** | Decoded as context; no routing/display consumer. |
| `draft/oper` | [oper tag](https://ircv3.net/specs/extensions/oper-tag) | **observe-only** | Decoded alongside unprefixed `oper`; no UI consumer. |
| Opaque unfamiliar tags | message-tags | **partial** | Preserved even when the name does not match today's grammar, copied into `MessageContext`, then stripped before ordinary legacy parsing; no descendant receiver currently uses the complete message. |

`TAGMSG` generates typing/reaction events and a complete `MessageContext`, then
the adapter deliberately omits legacy wire text. This applies even to a bare
`TAGMSG` after a server strips every tag, so it cannot reach unknown-command or
history handling. No descendant window currently turns those typed events into
visible typing/reaction UI.

### Batch types

The batch engine caps open batches, nesting depth, messages, per-batch bytes,
and total bytes (`portable/src/net/ircv3.cpp:1989-2172`; rejection/bounds tests at
`portable/tests/ircv3_test.cpp:669-787`).

| Opening type | Status | Evidence and missing behavior |
| --- | --- | --- |
| `chathistory` | **partial** | Produces `ChatHistory` and unwraps messages; legacy history/time/backfill semantics are absent. |
| `labeled-response` | **partial** | Label correlation/event exists; not every terminal response is adapted. |
| `draft/multiline` | **partial** | Receive-side concatenation exists; advertised limits and all invalid combinations are not enforced, and send-side batching is absent. |
| `netsplit`, `netjoin` | **observe-only** | Typed events exist for stable and accepted `draft/` aliases; no legacy member-list reconciliation. |
| `draft/isupport` / `isupport` | **partial** | Applies 005 items and emits events; replacement semantics need spec-focused tests. |
| `metadata`, `metadata-subs` | **observe-only** | Bounded state is updated before events; no product consumer. |
| `draft/chathistory-targets` | **observe-only** | Parsed as a history-family event; no target-picker or pagination UI. |
| Unknown batch type | **partial** | Structurally bounded and unwrapped. Safety still depends on the member commands being safe for legacy dispatch. |

There is no client-initiated `BATCH`/multiline builder. Opening-message tags
other than the label are not generally propagated to children. A multiline
batch is validated mainly against its first child rather than a normalized
opening target.

### ISUPPORT and ordinary IRC commands

`005` tokens are bounded, retained, removed on `-TOKEN`, and surfaced as
`Isupport` events (`portable/src/net/ircv3.cpp:1755-1837`). `CASEMAPPING`,
`MONITOR`, `MAXTARGETS`, and `TARGMAX` have typed behavior. Outbound
`CLIENTTAGDENY` and `UTF8ONLY` policy is enforced
(`portable/src/net/ircv3.cpp:1233-1268,2539-2658`; tests
`portable/tests/ircv3_test.cpp:621-667,840-883,1114-1132`).

| Identifier/category | Status | Evidence and boundary |
| --- | --- | --- |
| `CASEMAPPING` | **partial** | Portable retained maps are reindexed; the legacy model's separate nick/channel comparisons are not proven synchronized. |
| `MONITOR` | **partial** | Correctly treated as an ISUPPORT token, never a CAP. Limits/numerics/state are implemented; there is no product list-management UI. |
| `MAXTARGETS`, `TARGMAX` | **observe-only** | Bounded target lookup and removal are tested; legacy send fan-out does not consistently consult it. |
| `CLIENTTAGDENY` | **implemented** in engine | Client-only tag output is blocked/allowed according to the token and tested. |
| `UTF8ONLY` | **implemented** for output | Invalid outbound UTF-8 is rejected; this does not prove full legacy text conversion is Unicode-correct. |
| `draft/ICON` | **observe-only** | The [network-icon](https://ircv3.net/specs/extensions/network-icon) URL is exposed without fetching remote media, which is the safe default; no icon consumer. |
| `BOT` | **observe-only** | Retained generically; no member bot state/UI. |
| `WHOX`, `ACCOUNTEXTBAN`, `MSGREFTYPES`, `CHATHISTORY` | **observe-only** | Retained as generic tokens only; no builders, parsing policy, or pagination/reference behavior uses them. |
| Other well-formed 005 tokens | **observe-only** | Bounded preservation/removal and generic event only. |
| `PING`/`PONG` and CAP-unsupported fallback | **implemented** | Immediate PONG, keepalive correlation, `421 CAP`, and bounded timeout fallback are tested at `portable/tests/ircv3_test.cpp:167-187,886-921`. |
| `STARTTLS` / deprecated `tls` CAP | **unsupported by design** | The native transport connects with TLS from the start; do not add in-band downgrade-prone STARTTLS. |

## SASL and transport security

Primary sources: [SASL 3.2](https://ircv3.net/specs/extensions/sasl-3.2),
[PLAIN (RFC 4616)](https://www.rfc-editor.org/rfc/rfc4616),
[EXTERNAL (RFC 4422)](https://www.rfc-editor.org/rfc/rfc4422), and
[SCRAM-SHA-256 (RFC 7677)](https://www.rfc-editor.org/rfc/rfc7677).

Status: **partial** product support.

- `CAP LS 302` starts registration; SASL is requested only on a secure transport
  with usable credentials or explicit EXTERNAL opt-in
  (`portable/src/net/ircv3.cpp:1147-1214,1361-1373`).
- Mechanism preference is SCRAM-SHA-256, then EXTERNAL, then PLAIN. SCRAM uses
  mbedTLS PBKDF2/HMAC/SHA-256, bounds the iteration count, verifies the server
  signature in constant time, and fails closed (`portable/src/net/ircv3.cpp:398-462,907-1131`).
- Payload chunking, success/failure numerics, malformed challenges, RFC 7677
  vector behavior, authorization identities, signature failure, and secret
  wiping have direct tests (`portable/tests/ircv3_test.cpp:410-557,669-742`).
- The product deliberately sets `allow_external = false` because there is no
  client-certificate provisioning UI (`v2.5-beta-1-modern/ircsock.cpp:1078-1090`).
- The native mbedTLS transport requires certificate validation and hostname
  verification, with TLS 1.2 as the minimum
  (`portable/src/net/connection_engine.cpp:1141,1209-1210`).

Missing for full SASL 3.2 client support: post-registration reauthentication
after `CAP NEW sasl`, a product credential callback after secrets are wiped,
channel-binding/`-PLUS` mechanisms, a configured EXTERNAL certificate path, and
end-to-end UI reporting for every terminal outcome.

## Strict Transport Security

Primary source: [STS](https://ircv3.net/specs/extensions/sts).

Status: **partial**.

- STS is observed but never sent in `CAP REQ`.
- On plaintext, a valid `port` policy causes the adapter to discard all outbound
  responses from that line and reconnect securely before restarting CAP/SASL
  (`portable/src/net/ircv3.cpp:1452-1486`,
  `v2.5-beta-1-modern/ircsock.cpp:811-829`).
- Secure `duration`/`preload` values are parsed; insecure connections cannot set
  persistence. Tests cover parsing and `CAP DEL` behavior
  (`portable/tests/ircv3_test.cpp:189-252`).

The policy lives only inside one `Engine` instance. There is no durable,
per-host STS cache, expiry scheduler, preload bootstrap, or reconnect-time lookup.
Consequently secure `duration` is not enforced across client restarts or fresh
connections. This is a release-blocking security gap if the product advertises
full STS. `CAP DEL sts` must continue not to clear a stored policy; only the
spec-defined secure `duration=0` update may do that.

## Server profiles and fixtures

Profile detection is behavior-neutral: the `004` version string maps to a
`ServerIdentity` event (`portable/src/net/ircv3.cpp:184-194,2441-2448`), tested at
`portable/tests/ircv3_test.cpp:840-860`. There are no profile consumers that
alter parsing or request policy. This avoids accidental vendor forks, but it
also means the `bot` comment in the catalog is not an Orochi-only rule.

| Profile | Curated fixture coverage | What the fixture proves | What it does not prove |
| --- | --- | --- | --- |
| Solanum | `portable/tests/fixtures/irc/solanum.txt` | Version detection; CASEMAPPING/MONITOR/TARGMAX; CAP/SASL drive; tagged PRIVMSG; chathistory batch | Live Solanum interop, extended-JOIN legacy adaptation, CHATHISTORY pagination, or network policy |
| UnrealIRCd | `portable/tests/fixtures/irc/unrealircd.txt` | Unreal 6 identity; ASCII casemap; `bot`-tagged message; labels and history shape | Correct semantics for the non-standard offered `bot` CAP, services/account flows, or third-party invites |
| ircu | `portable/tests/fixtures/irc/ircu.txt` | `u2.*` detection and `421 CAP` fallback to ordinary IRC | Modern extensions (the fixture intentionally has none), encoding behavior, or every ircu derivative |
| Orochi | `portable/tests/fixtures/irc/orochi.txt` | Identity; filtering tag/batch/ISUPPORT names out of CAP requests; vendor CAP tolerance; netsplit shape | That every advertised vendor extension is supported. Its old `+draft/reply` spelling is preserved but not typed. |
| InspIRCd | `portable/tests/fixtures/irc/inspircd.txt` | Identity; CAP/SASL; account tag/away CAP shape; tagged history | Modules not present in the fixture, account UI behavior, or real deployed ordering |

The fixture harness auto-ACKs whatever the client requests and injects a SASL
success (`portable/tests/ircv3_test.cpp:982-1041`). It therefore proves bounded
parsing and registration completion, not that a real daemon would accept the
request set or that the legacy model handles negotiated message shapes.

## Flood, CTCP, SOUND, and DCC boundary

CTCP and DCC are Modern IRC conventions, not IRCv3 capabilities. Primary
sources: [CTCP](https://modern.ircdocs.horse/ctcp.html) and
[DCC](https://modern.ircdocs.horse/dcc). The current DCC document itself warns
that the specification is incomplete; classic DCC data connections are normally
unencrypted.

### Inbound admission

Status: **implemented with documented residual risks**.

The engine has global line/event buckets plus per-source event, CTCP, DCC, and
SOUND buckets, with bounded source tracking (`portable/src/net/ircv3.cpp:525-658`).
Batch members cannot bypass the specialized chat buckets. Tests cover ordinary,
CTCP, DCC, SOUND, batched, and global floods
(`portable/tests/ircv3_test.cpp:923-980`). Protocol-critical CAP, AUTHENTICATE,
PING, PONG, BATCH, SASL numerics, and retained-state messages bypass generic
admission so the connection can converge; their own maps/batches/state are
bounded.

Residual risks:

- Per-source keys use ASCII lowercase rather than the negotiated IRC casemap;
  aliases may split one sender across buckets, although global admission remains.
- CTCP classification checks the leading delimiter and command but does not
  require a closing `\x01` before applying the CTCP bucket.
- Limits are fixed implementation policy, not daemon-advertised flood budgets;
  outbound connection scheduling remains the separate transport responsibility.

### CTCP SOUND crash/path hardening

Status: **implemented** for the reported `/nul/nul.wav` class.

Remote names must be lexical basenames with an allowed audio extension. Rooted
paths, separators, traversal, controls/NUL, trailing dots/spaces, reserved DOS
device stems (including `NUL`, `CON`, `COM*`, and `LPT*`), missing files,
symlinks outside the sound root, and non-regular files are rejected
(`portable/src/sound.cpp:70-145`). The regression suite explicitly includes
`/nul/nul.wav` and device/path variants (`portable/tests/sound_test.cpp:31-105`).
The legacy CTCP path validates before playback
(`v2.5-beta-1-modern/protsupp.cpp:1310-1378`).

### DCC

Status: **partial**.

The modern adapter strictly parses DCC SEND offers, bounds sizes, validates
decimal IPv4/port values and address scope, rejects loopback/link-local/reserved
targets, checks a numeric identity address when available, and prompts for LAN
or unverifiable peers (`v2.5-beta-1-modern/filesend.cpp:221-252,714-752`). The
native libuv transfer engine has tests for SEND/RECEIVE streaming, cumulative
ACKs, capacity, cancellation, deadlines, overflow, restart, wakeup races, and
peer filtering (`portable/tests/dcc_transfer_test.cpp:47-454`).

There is no evidence for DCC CHAT, RESUME/ACCEPT, reverse/passive DCC, SDCC, or
transport encryption/authentication. Do not describe DCC SEND support as secure
file transfer; retain explicit consent and address-scope warnings.

## Test coverage limits

The standalone protocol test is strong on deterministic parsing, bounds, CAP
state, SASL cryptography, batches, retained state, output policy, and its bounded
event-drain helper. It does **not** instantiate the MFC channel/member/history
model. `TestLegacyUiEventBridge` proves bounded delivery and context preservation
only (`portable/tests/ircv3_test.cpp:1135-1189`); it does not prove any descendant
window handles the event.

Current conspicuous gaps include direct causal tests for `invite-notify`,
`multi-prefix`, `userhost-in-names`, `pre-away`, extended-JOIN normalization,
labeled no-response ACKs, persistent STS, CHATHISTORY
recovery invocation, event-playback isolation, visible redaction, and UI/model
application of every typed state event.

## Prioritized completion plan

### P0 — stop negotiating behavior the product cannot safely consume

1. Add a product-readiness request policy and default-disable `extended-join`,
   `multi-prefix`, `userhost-in-names`, `no-implicit-names`, `invite-notify`,
   `draft/event-playback`, `draft/channel-rename`,
   `draft/message-redaction`, and `draft/multiline` until their adapters pass.
2. **Complete at the parser/adapter boundary:** inbound message-tag keys are
   preserved opaquely without weakening outbound validation, and `TAGMSG` is
   typed-only before legacy dispatch. Visible consumers remain part of item 3.
3. Implement actual legacy event consumers for account/away/host/realname,
   rename, standard replies, redaction, typing/reaction, read markers, metadata,
   and message context; add model-level tests.
4. Add a durable, per-host STS policy store with expiry, secure `duration=0`
   removal, reconnect enforcement, and downgrade tests.

### P1 — complete negotiated feature semantics

1. Normalize extended JOIN, multiple NAMES prefixes, and userhost-in-names before
   legacy parsing; add daemon-shaped fixtures and MFC consumer tests.
2. Enforce every multiline opening target/limit/blank rule and add outbound
   client-batch/multiline construction.
3. Wire CHATHISTORY recovery/pagination into reconnect without replaying
   uncertain messages or treating event playback as live state.
4. Complete labeled-response terminal handling, use labels as the primary echo
   identity, and expose standard replies instead of silently consuming them.
5. Add SASL reauthentication/credential callbacks and an explicit client-
   certificate configuration path before enabling EXTERNAL in the product.

### P2 — user-facing and interoperability depth

1. Add account registration, metadata, MONITOR, read-marker, reply/reaction,
   typing, bot/oper status, network-icon, and history UI only after their data
   lifetime and privacy policies are defined.
2. Capture sanitized live transcripts for the five supported daemon families
   and test negative/ordering variants; keep curated fixtures clearly labeled.
3. Extend DCC only behind explicit threat models; prioritize safer external
   transfer mechanisms over adding unauthenticated DCC variants.
4. Re-run this ledger against the IRCv3 index before each release. Preserve
   draft prefixes until the official specification changes, then migrate with
   compatibility tests rather than silently accepting premature unprefixed use.
