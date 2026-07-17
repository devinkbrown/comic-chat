# IRC and IRCv3 coverage

Audit snapshot: 2026-07-17, `main` at `1adea2d`.

> **Archival note:** The `v1.0-pre-modern` Windows lane cited in some entries
> below has been **archived** to the `version/v1.0-pre-modern` branch and is no
> longer on `main`. Reinked's current shape is one version with two UIs
> (`v2.5-beta-1-modern` + `portable`) over the shared `portable/src` core. Any
> `v1.0-pre-modern/…:line` citation here refers to that archival branch, and the
> "v1" completion entries record work that landed on it before it was archived.

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
comicchat-ircv3 --print-errorlogs`; it passes 1/1 at this `1adea2d` snapshot,
and the full `meson test -C portable/build` suite passes 11/11, both re-executed
against this commit rather than inherited from the previous audit. Each cited
test was also re-verified as registered in `portable/meson.build`. The five
server files are curated transcript fixtures, not live-server certification.

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
5. `CChatView::OnIrcv3Event` resolves each receiving document's own user model
   and applies bounded account, away, host, and realname mutations on the UI
   thread. Away/back deliberately reuses Microsoft's original `DoUserAway`
   remove/mutate/reinsert path so the member icon repaints
   (`v2.5-beta-1-modern/ircv3eventbridge.h:25-110`,
   `v2.5-beta-1-modern/chatview.cpp:85-131`).

Step 5 remains the principal completion gap, but it is no longer absent. The
identity events above have causal MFC-independent model tests. Standard
`FAIL`/`WARN`/`NOTE` descriptions also reach Microsoft's existing status view
once, through the focused document. Channel rename preserves the existing room
document/member/topic/mode state while updating its tab/path/status and queued
legacy query targets. Redaction, typing/reaction, read markers, metadata, and
complete message contexts still lack legacy view/model consumers. A typed
portable event is not, by itself, legacy-client support.

## Capability request safety

Wire support and product readiness are now separate executable policies.
`CapabilityCatalog()` defaults incomplete or observe-only product features off;
`Engine::SetCapabilityRequestEnabled()` permits a frontend with a completed
adapter to opt a known capability in or out without allowing STS, implicit
`cap-notify`, unknown identifiers, tags, commands, batch types, or ISUPPORT
tokens to become `CAP REQ` entries (`portable/src/net/ircv3.cpp:472-535,
1265-1271,1402-1418`). Normative dependencies and the verified-TLS SASL gate
still apply after an opt-in. Gated offers remain discoverable through
`IsOffered()`/`CapabilityValue()` and parsing stays liberal and bounded.

The default request set is deliberately small: message tags, server time,
account tag/notify, away notify, chghost, setname, standard replies, secure
usable SASL, and implicit CAP 302 state. Generic batch, the non-standard bot CAP,
echo/labeled response, history, metadata, read markers, monitoring, and all
known wire-shape hazards default off. Tests prove both safe defaults and explicit
dependency-respecting opt-in (`portable/tests/ircv3_test.cpp:207-388`).

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
| `batch` | stable | **partial** | Bounded nesting and several batch families in `portable/src/net/ircv3.cpp:1989-2172`; tests `portable/tests/ircv3_test.cpp:313-348,744-787` | **Default off.** Opt in only with audited member semantics; a generic batch can unwrap a command the legacy model cannot safely consume. |
| `server-time` | stable | **observe-only** | `time` is preserved as an unknown/generic tag and stripped after `MessageContext`; Solanum/Unreal/Orochi/InspIRCd fixtures | Safe for ordinary chat, but the legacy history timestamp is not set from the tag. |
| `standard-replies` | stable | **implemented** | `FAIL`/`WARN`/`NOTE` become bounded typed events; the focused document formats their required descriptions into Microsoft's status view with severity color, while malformed tokens/NUL/oversize input no-op (`v2.5-beta-1-modern/ircv3eventbridge.h`, `chatview.cpp`) | Safe to request; every well-formed reply remains visible without exposing raw tags or duplicating it across rooms. |
| `cap-notify` | stable | **implemented** | Implicit under CAP 302; `NEW`/`DEL`, dependent removal, and request tracking in `portable/src/net/ircv3.cpp:1488-1615`; tests `portable/tests/ircv3_test.cpp:189-252` | Safe. Never remove the implicit state merely because a server sends `CAP DEL cap-notify`. |
| `account-notify` | stable | **partial** | Bounded account state and `Account` event feed the document-owned `CUserInfo::SetAccount` path; parser/state and causal fake-model tests cover login/logout and unknown users | Safe to request. Account identity is retained separately, but account-aware display and moderation remain future work. |
| `account-tag` | stable | **observe-only** | Tag is preserved in `MessageContext`; fixture coverage, but no typed field or legacy use | Safe to request because the ordinary legacy wire shape is preserved; do not claim account-aware display or moderation. |
| `away-notify` | stable | **implemented** | Bounded away state reaches `DoUserAway` on the UI thread, preserving Microsoft's original member-list refresh behavior; state/bound and causal model tests | Safe to request. |
| `bot` | non-standard vendor CAP | **observe-only** | Offer is retained and the `bot` tag decodes; Orochi and Unreal fixtures cover discovery | **Default off.** The standard bot-mode contract is the `bot` tag plus `BOT` ISUPPORT, not a general `bot` capability. |
| `chghost` | stable | **partial** | Bounded host state and `HostChanged` update the document-owned historical `user@host` identity; state/bound and causal model tests | Safe to request; a dedicated identity display remains absent. |
| `echo-message` | stable | **partial** | Outgoing labels/content are retained and inbound echoes can be suppressed; tests cover correlation | **Default off.** Content-only fallback can mistake an identical echo from another bouncer session for the local message; durable pending-message identity is required. |
| `extended-join` | stable | **partial** | The adapter flattens the three-parameter JOIN to the channel alone before the legacy handler, keeping the complete message for typed consumers (`v2.5-beta-1-modern/ircv3eventbridge.h:457-466,493`); the engine rejects every other shape, bounds account/realname state, and emits `Account`/`RealnameChanged` (`portable/src/net/ircv3.cpp:2431-2488`); causal test `v2.5-beta-1-modern/tests/transport_ui_bridge_test.cpp:340` covers the channel key, the preserved typed identity, and the malformed-shape rejection | **Still default off** (`portable/src/net/ircv3.cpp:539`). The realname can no longer become the channel key, and account/realname reach the same document-owned fields as `account-notify`/`setname`, so the catalog precondition is met. It stays partial for the same reason those rows do: no visible account or realname surface consumes them. |
| `invite-notify` | stable | **unsupported** | Catalog presence only. No adapter and no causal test; `INVITE` appears in the bridge solely as a trailing-parameter rule (`v2.5-beta-1-modern/ircv3eventbridge.h:365`), which is a wire-shape concern and not third-party-invitation handling | **Block.** Unchanged. The legacy INVITE path assumes an invitation for the local user (`v2.5-beta-1-modern/ircsock.cpp:1361-1367`), and nothing distinguishes a third-party notification from one addressed to the local user. |
| `labeled-response` | stable | **partial** | Labels, batch correlation, echo correlation, and terminal no-parameter `ACK` handling; malformed/unlabeled ACK is contained as a typed protocol error (`portable/src/net/ircv3.cpp:2521-2545`) | **Default off.** ACK no longer reaches legacy unknown-command handling, but full response presentation and durable pending-message identity are unfinished. |
| `multi-prefix` | stable | **partial** | `NormalizeLegacyNamesReply` strips every leading prefix symbol from each `353` token and re-emits at most one symbol, collapsing higher server ranks to the four roles Microsoft's member model can represent — owner `.`, operator `@`, voice `+`, spectator `>` (`v2.5-beta-1-modern/ircv3eventbridge.h:399-446`); it validates the `PREFIX` token and fails closed on a malformed one; direct test with `PREFIX=(qaohv)~&@%+` at `v2.5-beta-1-modern/tests/transport_ui_bridge_test.cpp:364` | **Still default off** (`portable/src/net/ircv3.cpp:546`). `CUserInfo` still strips only one prefix (`v2.5-beta-1-modern/userinfo.cpp:121-144`), but normalization now runs upstream, so `@+nick` reaches it as `@nick`, not `+nick`. Partial, not implemented: the collapse discards ranks the legacy model could otherwise hold as distinct flags, so `@+nick` loses `UF_HASVOICE`. |
| `no-implicit-names` | stable | **partial** | Both legacy self-JOIN sites now gate on `m_ircEngine.IsEnabled("no-implicit-names")` and queue an explicit request beside the response-tracking `CCQuery`, failing closed via `OnClose(WSAENOBUFS)` when the line cannot be queued (`v2.5-beta-1-modern/ircsock.cpp:1378-1385,1536-1543`); the bounded builder rejects empty, spaced, comma-bearing, NUL-bearing, and oversize channel names (`v2.5-beta-1-modern/ircv3eventbridge.h:194`); causal test `v2.5-beta-1-modern/tests/transport_ui_bridge_test.cpp:395` | **Still default off** (`portable/src/net/ircv3.cpp:549`). It does now transmit `NAMES`, so the catalog precondition is met and a suppressed implicit reply no longer leaves the member list empty. Partial, not implemented: the builder is tested in isolation, but no legacy-consumer test proves the request is issued on JOIN — that path is MFC and CI-only. |
| `sasl` | stable | **partial** | PLAIN, explicit EXTERNAL, and SCRAM-SHA-256; see dedicated section | Safe only on verified TLS with a usable mechanism. Product has no client-certificate provisioning and no post-registration reauthentication. |
| `setname` | stable | **partial** | `SETNAME` state and `RealnameChanged` update a separate document-owned realname field; state/bound and causal model tests | Safe to request; a visible realname surface remains absent. |
| `userhost-in-names` | stable | **partial** | `NormalizeLegacyNamesReply` truncates each token at `!` so the nickname column stays clean, and now also splits the hostmask and carries it as flat identity triples in `Event::context`; `ConsumeNamesIdentities` applies each one through the same host mutation as `chghost`, reaching `CUserInfo::SetFullName` and giving `MatchesNickMask` real data (`v2.5-beta-1-modern/ircv3eventbridge.h`, `v2.5-beta-1-modern/chatview.cpp`). Causal test `TestUserhostInNamesSuppliesHostmaskToLegacyModel` covers hostmask delivery, prefixed-plus-hostmask tokens, tokens without a hostmask, and fail-closed rejection without partial writes; it was mutation-tested to confirm each case is causal | **Still default off** (`portable/src/net/ircv3.cpp:554`). The catalog precondition is now met: NAMES adaptation splits `nick!user@host` and supplies the hostmask separately. Partial, not implemented: the `chatview.cpp` consumer is MFC and compiles only under MSVC CI, and identities outside `qpInitialNames` (a later `/names`) resolve to `unknown_user`, so the capability's reach is join-time membership only. |
| `draft/channel-rename` | work in progress | **partial** | The engine requires both channels plus the mandatory, possibly empty reason and migrates retained state. The legacy UI keeps the same room/member/topic/mode document, updates encoded/pretty names, tab/path/status, rewrites pending query targets, and displays the reason; malformed input and local target collisions fail closed | **Default off while draft.** The product adapter is present and causal-tested, but native MFC compilation is CI-only and a user-facing policy for an already-open local target tab remains unresolved. |
| `draft/account-registration` | work in progress | **partial** | Secret-consuming `REGISTER`/`VERIFY` builders and typed outcomes in `portable/src/net/ircv3.cpp:2595-2658`; tests `portable/tests/ircv3_test.cpp:1083-1112` | Block until an account UI consumes the API and replies. |
| `draft/chathistory` | work in progress | **partial** | History batches and bounded recovery command builder in `portable/src/net/ircv3.cpp:1315-1325,1989-2111`; tests `portable/tests/ircv3_test.cpp:398-408,744-787` and four CAP fixtures | **Block for now.** No production caller uses `RecoveryCommands()`. Requesting chathistory can suppress server auto-playback without replacing it. |
| `draft/event-playback` | work in progress | **partial** | Dependency and generic batch unwrapping; no historical/live distinction at legacy boundary | **Block.** Historical JOIN/MODE/NICK/etc. can be replayed into the legacy model as live state. |
| `draft/extended-isupport` | work in progress | **partial** | `isupport`/`draft/isupport` batches and bounded generic 005 state in `portable/src/net/ircv3.cpp:1755-1837,1989-2111`; tests `portable/tests/ircv3_test.cpp:621-667,744-787` | Keep gated until batch replacement/removal semantics and legacy consumers are causal-tested. |
| `draft/metadata-2` | work in progress | **observe-only** | Bounded values/subscriptions, numerics, commands and batches in `portable/src/net/ircv3.cpp:1887-1949,1989-2111`; tests `portable/tests/ircv3_test.cpp:1044-1081` | Block until product consumers and outbound UI exist. |
| `draft/message-redaction` | work in progress | **observe-only** | Retained IDs and `Redaction` event; state/bound tests | **Block.** No handler removes or marks the rendered balloon, so advertised support leaves redacted content visible. |
| `draft/multiline` | work in progress | **partial** | Receive-side reassembly and `Multiline` event; `PrepareMultiline` builds client-initiated batches, enforcing the capability gate, a single target, advertised `max-lines`/`max-bytes` counted over content only and clamped by the engine's own bounds, the blank-line concat prohibition, a unique monotonic ref, and rejection rather than truncation of over-long lines (`portable/src/net/ircv3.cpp:2980-3070`); receive-side children validate against the normalized opening target, and first-child concat is ignored as the no-op the spec leaves undefined rather than dropping the batch (`portable/src/net/ircv3.cpp:2262-2295`); causal tests `portable/tests/ircv3_test.cpp:490-580,594-680` | **Default off while draft.** The builder and its limits are now enforced and mutation-tested. Still partial: no `FAIL BATCH MULTILINE_*` standard replies are emitted on violation (errors surface as `ParseFailure`), and no production caller sends a multiline message. |
| `draft/oper-tag` | work in progress | **observe-only** | `draft/oper` and unprefixed `oper` decode to typed context; typed-tag tests | Block until the renderer consumes the status. Do not emit or prefer the unprefixed tag while the spec remains draft. |
| `draft/pre-away` | work in progress | **unsupported** | Catalog entry only; no pre-registration AWAY builder or causal test | **Block.** Negotiation without use supplies no product feature. |
| `draft/read-marker` | work in progress | **observe-only** | Bounded `MARKREAD` state and `ReadMarker` event; state/bound tests | Block until read state is displayed and outbound advancement is implemented. |
| `extended-monitor` | stable | **observe-only** | Presence numerics/state and typed events, generic outgoing path; `portable/src/net/ircv3.cpp:1755-1885`; tests `portable/tests/ircv3_test.cpp:621-667` | Block until the UI manages a MONITOR list. This CAP does not replace the base `MONITOR` ISUPPORT token. |
| `sts` | stable, server-advertised | **partial** | Observed outside the request catalog; immediate plaintext upgrade and a durable per-host policy substrate exist; see dedicated section | Correctly never request. Do not claim durable enforcement until every production session owns the store lifecycle. |

### Request-policy gate

**Implemented.** The engine now has a safe default product-readiness policy and
an explicit known-capability override. Parsing remains liberal and bounded when
requesting is disabled, dependencies cannot be bypassed, and server profiles do
not silently opt features in. A frontend must ship its consumer and causal tests
before it enables a gated capability.

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
| `label` | [labeled response](https://ircv3.net/specs/extensions/labeled-response) | **partial** | Correlated to requests, opening batches, and terminal ACK; no full legacy response presentation. |
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
| `labeled-response` | **partial** | Label correlation/event and no-output ACK exist; not every response is presented by the legacy UI. |
| `draft/multiline` | **partial** | Receive-side concatenation exists and validates every child against the normalized opening target; send-side batching now exists via `PrepareMultiline` with advertised limits enforced. Remaining: no `FAIL BATCH MULTILINE_*` emission and no production caller. |
| `netsplit`, `netjoin` | **observe-only** | Typed events exist for stable and accepted `draft/` aliases; no legacy member-list reconciliation. |
| `draft/isupport` / `isupport` | **partial** | Applies 005 items and emits events; replacement semantics need spec-focused tests. |
| `metadata`, `metadata-subs` | **observe-only** | Bounded state is updated before events; no product consumer. |
| `draft/chathistory-targets` | **observe-only** | Parsed as a history-family event; no target-picker or pagination UI. |
| Unknown batch type | **partial** | Structurally bounded and unwrapped. Safety still depends on the member commands being safe for legacy dispatch. |

`PrepareMultiline` is the only client-initiated `BATCH` builder; there is no
general one for other batch types. Opening-message tags other than the label
are not generally propagated to children. A multiline batch is now validated
against its normalized opening target rather than its first child.

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
  (`portable/src/net/ircv3.cpp`, `portable/src/net/sts_session.cpp`,
  `v2.5-beta-1-modern/ircsock.cpp`). The session coordinator consumes the typed
  update and owns both callbacks, so an upgrade callback runs instead of—not
  after—the same line's event/message/outbound callback.
- Secure `duration`/`preload` values are parsed; insecure connections cannot set
  persistence. The engine now emits typed upgrade, persist, and remove actions;
  duplicate normative STS keys and duplicate `sts` capability tokens are
  rejected. Tests cover parsing, typed actions, capability advertisements, and
  `CAP DEL` behavior
  (`portable/tests/ircv3_test.cpp:189-280`).
- `StsPolicyStore` provides a portable, hostname-keyed durable cache contract.
  It bounds files to 256 KiB, entries to 1,024, and hostname keys to 255 bytes;
  parses into a replacement snapshot; rejects symlink/reparse and unsafe Unix
  files; and commits through bounded unique, exclusive, same-directory
  temporary files plus an atomic replacement (mode `0600` on Unix and the
  caller's private-directory ACL on Windows). Its pre-start
  `ConnectionOptions` plan forces TLS and the last verified secure port, so
  retrying a failed TLS connection cannot select plaintext. Focused tests cover
  restart, exact expiry, secure `duration=0`, overflow, malformed/oversized and
  over-count files, temporary residue/collision, and no-downgrade planning
  (`portable/tests/sts_policy_store_test.cpp`).
- The v2.5 Windows session is now wired end to end. It obtains
  `FOLDERID_LocalAppData` through the native shell API, creates/rejects the
  application directory without a `HOME` fallback, loads before every external
  transport start, and routes the exact `StsPolicyStore::plan` result into
  `ConnectionEngine::start`. Verified TLS Persist/Remove updates use the
  requested hostname, planned secure port, and current generation; the last
  receipt is rescheduled and cleared at each verified disconnect. Internal
  retries retain the same TLS-only plan, while replacement starts are replanned
  (`portable/include/comicchat/net/private_config.hpp`,
  `portable/include/comicchat/net/sts_session.hpp`,
  `v2.5-beta-1-modern/ircsock.cpp`).
- MFC-independent causal tests cover restart/no-downgrade, same-generation
  retry, stale-generation isolation, secure `duration=0`, persistence before
  output, plaintext upgrade instead of output, and unreadable/write-failure
  fail-closed behavior. The same test is registered in Meson and the native
  NMAKE/Windows CI lane (`portable/tests/sts_session_test.cpp`).

Full STS remains **partial**, but no longer because the production sessions are
missing. All three now construct a policy owner: the Unix/BSD frontend builds a
`NativeSession` over the resolved policy file (`portable/src/app.cpp:211-215`,
`portable/src/net/native_session.cpp:673`), and the v1 legacy session plans
through `StsSession` before transport start (`v1.0-pre-modern/irc.cpp:1121-1187`).
It remains partial because the policy lifecycle is not yet exercised end to end
on every frontend, and because a preload bootstrap is intentionally absent.
`CAP DEL sts` must continue not to clear a stored policy; only the spec-defined
secure `duration=0` update may do that.

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
model. The bridge tests prove bounded delivery, context preservation, and causal
account/away/host/realname mutation against an MFC-independent fake model. The
native handler is wired, but this Linux audit cannot instantiate MFC; its actual
Windows compilation remains an MSVC CI gate.

Current conspicuous gaps include direct causal tests for `invite-notify` and
`pre-away`, a legacy-consumer test for the explicit NAMES request on JOIN, full
labeled response presentation, `FAIL BATCH MULTILINE_*` standard replies and a
production caller for the multiline builder, CHATHISTORY recovery invocation,
event-playback isolation, visible redaction, and UI/model application of the
remaining typed state events.

Closed since the previous snapshot: extended-JOIN normalization, multi-prefix
NAMES normalization and its direct test, explicit NAMES transmission under
`no-implicit-names`, hostmask delivery to the legacy user model for
`userhost-in-names`, client-initiated multiline batch construction, and
production STS session wiring across all three frontends. Normalization is not
negotiation — all five NAMES/JOIN-adjacent capabilities below remain default-off
in the catalog.

Three defects that formerly blocked the gated NAMES/JOIN capabilities -- found
by audit rather than by test failure, since the tests then asserted the
defective behavior was correct -- are now fixed, each with a causal test that
fails against the old behavior:

- `extended-join` at the identity-state bound now delivers the member and drops
  only the account/realname annotation, instead of swallowing the whole JOIN
  (`portable/src/net/ircv3.cpp` `HandleJoinMessage`; tests
  `portable/tests/ircv3_test.cpp`, "extended JOIN at the state bound still
  delivers the member"). PART pruning was deliberately *not* added: the engine
  tracks only its own channels, so pruning a PART would drop identity for a user
  still visible in another shared channel. The maps remain bounded at
  `kMaxStateEntries`, so growth is capped and the former silent member-drop is
  gone; a fuller fix would refcount per-user membership.
- `no-implicit-names` in `v1.0-pre-modern` now recovers the channel from the
  JOIN's own parameters via the testable `LegacyJoinChannel`
  (`v1.0-pre-modern/transportadapter.h`; test
  `TestNoImplicitNamesQueriesTheJoinedChannel`), so a server forward queries the
  room we actually landed in. The one-line call-site wiring in
  `v1.0-pre-modern/irc.cpp` is MFC and compiles only under MSVC CI.
- An empty but legitimate `PREFIX=` ISUPPORT token now normalizes with an empty
  prefix set -- stripping nothing, still splitting hostmasks -- instead of
  discarding the whole `353` (`v2.5-beta-1-modern/ircv3eventbridge.h`
  `NormalizeLegacyNamesReply`; test in
  `TestNamesExtensionsNormalizeBeforeLegacyMembership`).

The catalog default remains global across two frontends with different adapters
and coverage. `Engine::SetCapabilityRequestEnabled`
(`portable/src/net/ircv3.cpp:1382`) is the per-frontend lever this ledger's
request-policy gate describes, and no production frontend calls it. Wire it
before any default flips. All five NAMES/JOIN capabilities remain default-off.

## Prioritized completion plan

### P0 — stop negotiating behavior the product cannot safely consume

1. **Complete:** product readiness is separate from wire support; incomplete
   response, batch, history, metadata, monitoring, draft, and wire-shape
   features default off and require explicit adapter-owned opt-in.
2. **Complete at the parser/adapter boundary:** inbound message-tag keys are
   preserved opaquely without weakening outbound validation, and `TAGMSG` is
   typed-only before legacy dispatch. Visible consumers remain part of item 3.
3. **Partially complete:** account/away/host/realname have document-owned UI-
   thread consumers and model-level tests, and standard replies reach the
   existing status view; channel rename preserves and retargets the legacy room.
   Implement redaction, typing/reaction, read markers, metadata, and message
   context.
4. **Complete for v2.5 Windows, the Unix/BSD frontend, and v1:** the durable
   per-host policy lifecycle is wired into every production session. Commit
   `70f6ad3` closed the v1 gap — `CIrcSocket::StartConnection` loads the policy,
   plans through `sts_session_->start(...)`, and only then calls
   `connection_.start(std::move(planned))` (`v1.0-pre-modern/irc.cpp:1121-1187`),
   with `comicchat-v1-transport-adapter` registered in the build
   (`portable/meson.build:488-493`). All paths plan before transport start,
   commit verified secure updates/removals, retain only a current connection
   receipt for disconnect rebasing, and have adapter-level downgrade tests.
   Remaining: the preload bootstrap noted above.

### P1 — complete negotiated feature semantics

1. Extended JOIN and multiple NAMES prefixes are normalized before legacy
   parsing and directly tested; userhost-in-names is truncated safely but its
   hostmask is discarded. Supply that hostmask to the legacy user model, add
   daemon-shaped fixtures, and add MFC consumer tests before flipping any of
   these defaults on.
2. Enforce every multiline opening target/limit/blank rule and add outbound
   client-batch/multiline construction.
3. Wire CHATHISTORY recovery/pagination into reconnect without replaying
   uncertain messages or treating event playback as live state.
4. Terminal labeled ACK handling and standard-reply presentation are complete.
   Use durable labels as the primary echo identity for multi-session bouncers.
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
