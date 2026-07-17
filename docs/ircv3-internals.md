# IRCv3 subsystem internals

Audience: contributors working on the networking stack. This document describes
how the IRCv3 subsystem is *built* — the pipeline, the layering, and the load-
bearing invariants — so you can change it without breaking the trust boundaries
that hold it together. For per-capability *status* (implemented / partial /
observe-only / unsupported), read [`IRCv3-COVERAGE.md`](IRCv3-COVERAGE.md); this
file does not repeat that ledger and deliberately avoids restating which features
are "done." Every structural claim below is cited to source you can open.

> Line numbers are given as `file:line` against the tree at the time of writing.
> The engine file is large and actively edited; if a citation has drifted, search
> for the named symbol rather than trusting the number. (The coverage ledger's own
> anchors have already drifted — see [What is NOT done](#what-is-not-done).)

> **Archival note (2026-07-17):** The `v1.0-pre-modern` Windows lane cited below
> has been **archived** to the `version/v1.0-pre-modern` branch and is no longer
> on `main`. Reinked's current shape is one version with two UIs
> (`v2.5-beta-1-modern` + `portable`) over the shared `portable/src` core. Any
> `v1.0-pre-modern/…:line` citation here refers to that archival branch; the v1
> adapter described below lives on it.

## The shape of the subsystem

There are three layers, and they are physically separated so the innermost one
never learns about the outermost:

1. **The portable engine** — `portable/src/net/ircv3.cpp` +
   `portable/include/comicchat/net/ircv3.hpp`. Pure, MFC-free,
   post-C++23/26 (the header hard-errors on an older language mode,
   `ircv3.hpp:4-10`). It turns wire bytes into bounded `Message`s, negotiates
   CAP 302, and returns typed `Event`s plus outbound command strings. It holds
   bounded protocol state but knows nothing about the UI.
2. **The capability-gating policy** — the `CapabilityCatalog()` table
   (`ircv3.cpp:516-579`) plus the per-frontend override lever
   `Engine::SetCapabilityRequestEnabled` (`ircv3.cpp:1382-1389`). This is where
   *wire support* and *product readiness* are kept as two separate executable
   decisions.
3. **The legacy bridge** — `v2.5-beta-1-modern/ircv3eventbridge.h` and
   `v1.0-pre-modern/transportadapter.h`. Header-only adapters that flatten the
   modern wire into what the 1998 MFC parser expects, and translate the engine's
   typed events into mutations on the document-owned `CUserInfo` model.

The data flow in one line: socket bytes → `LineFramer` → `Engine::Process` →
`ProcessResult{messages, outbound, events, sts_update}` → the bridge splits that
into (a) flattened legacy wire fed to Microsoft's parser and (b) typed events
applied to the UI model on the UI thread.

---

## 1. The portable engine

### 1.1 Wire line → bounded `Message`

`Message` (`ircv3.hpp:64-79`) is `{tags, prefix, command, params}`. Parsing has
two entry points that share one implementation: the throwing/`expected` form
`Message::Parse(wire)` (`ircv3.cpp:806-812`) delegates to the `bool
Parse(wire, out, error)` overload (`ircv3.cpp:726-804`).

Bounds are enforced *during* parse, not after, and everything is a named
constant at the top of the file (`ircv3.cpp:24-53`):

- CRLF is stripped tolerantly — LF-only and bare-CR are both accepted
  (`ircv3.cpp:729-734`).
- Any control byte fails the whole line closed (`HasLineControl`,
  `ircv3.cpp:742`).
- A tag frame is capped at `kMaxTagFrameBytes` (8191) and the message body at
  `kMaxMessageBytes` (510) (`ircv3.cpp:745-746, 777`). Tag keys are parsed as
  **views into the bounded wire buffer** and de-duplicated through an
  `unordered_map<string_view,…>` so a unique-tag fanout is O(n) with no second
  allocation; on a duplicate key the last occurrence wins
  (`ircv3.cpp:753-772`). Inbound keys are treated as opaque and case-sensitive —
  the parser deliberately does **not** reject a message because a key fails
  today's grammar (`ircv3.cpp:763-766`); `ValidTagKey` is applied only on
  *outbound* serialization (`SerializeChecked`, `ircv3.cpp:825`).
- At most `kMaxParams` (15) params; the trailing `:`-param captures the
  remainder (`ircv3.cpp:790-801`).

`SerializeChecked` (`ircv3.cpp:814-865`) is the inverse and is where outbound
validity is actually enforced (valid command, param count, no control bytes,
tag-key grammar, 512-byte frame). Credentials are wiped from serialization
scratch buffers via `ScopedStringWipe`/`IsSensitiveCommand`
(`ircv3.cpp:820, 841`).

`LineFramer` (`ircv3.hpp:201-212`) sits in front of `Process` and splits the TCP
stream into lines with its own byte ceiling (default `8191 + 512`).

### 1.2 Registration and the `Process` loop

`BeginRegistration` has three overloads (`ircv3.hpp:224-239`): an lvalue
`SaslConfig` (copied, caller keeps ownership), an rvalue overload that
**overwrites the source SSO storage** so short-string credentials cannot survive
in a moved-from buffer, and a third that takes a `SharedLockedSecret` lease on
already page-locked password bytes (the config password must then be empty —
supplying both is rejected fail-closed).

`Engine::Process(wire)` (`ircv3.cpp:2684+`) is the single inbound entry point and
returns a move-only `ProcessResult` (`ircv3.hpp:186-199`) carrying
`messages`, `outbound`, `events`, and an optional `sts_update`. The dispatch
order inside `Process` is itself a policy:

1. Parse; a parse failure becomes a typed `ProtocolError` event, never a throw
   (`ircv3.cpp:2688-2693`).
2. `PONG`/`PING` keepalive handled inline before anything else
   (`ircv3.cpp:2695-2710`).
3. **Flood admission** (`flood_->Admit`, `ircv3.cpp:2711-2725`). The
   `FloodController` (`ircv3.cpp:590+`) token-buckets non-critical lines; a
   suppressed line becomes an `Abuse` event and is dropped. Critical commands
   (PING/PONG/CAP/AUTHENTICATE/BATCH/ERROR and the retained-state commands) and
   batch members bypass the bucket (`ircv3.cpp:598-611`).
4. JOIN-state, then CAP, AUTHENTICATE, BATCH, then batch-tagged members, then
   ordinary state/tag handling (`ircv3.cpp:2726-2730+`).

CAP 302 negotiation lives in `HandleCap` (`ircv3.cpp:1698+`): it accumulates the
multi-line `CAP LS` into `ls_accumulator_`, then `ParseCapabilityList`
(`ircv3.cpp:1480-1494`) records offers, `SelectCapabilities`
(`ircv3.cpp:1514-1531`) picks what to request, `RequestCapabilities`
(`ircv3.cpp:1533+`) batches `CAP REQ` lines, and `MaybeFinishRegistration`
(`ircv3.cpp:1559+`) sends `CAP END` once nothing is pending. Two fallbacks close
registration for servers that ignore CAP: `FinishRegistrationWithoutCapabilities`
(`ircv3.cpp:1432-1445`) and the bounded-timeout `FinishRegistrationAfterTimeout`
(`ircv3.cpp:1447-1453`), which only emits `CAP END` if the server was ever seen
to understand CAP.

### 1.3 Retained bounded state

The engine keeps small server-driven maps/sets — accounts, away messages, hosts,
realnames, read markers, metadata, redactions, joined channels, ISUPPORT,
monitor lists, target limits, STS policy, case mapping (`ircv3.hpp:365-409`),
each exposed through a const accessor (`ircv3.hpp:277-296`). Every one is bounded
by a `kMax*` constant (`ircv3.cpp:41-53`). Case mapping changes trigger a
`ReindexState` (`ircv3.hpp:363`) so keys stay consistent when the server's
`CASEMAPPING` differs from the default `Rfc1459`.

### 1.4 SASL: PLAIN / EXTERNAL / SCRAM-SHA-256

SASL is an inner `Engine::SaslSession` class (`ircv3.cpp:1000-1099+`). Mechanism
selection is preference-ordered in `Start` (`ircv3.cpp:1030-1035`):
SCRAM-SHA-256 first when an id+password exist and the server advertises it, then
EXTERNAL if `allow_external` is set (client-cert auth — callers must opt in after
provisioning a cert, `ircv3.hpp:126-128`), then PLAIN. If the server advertises a
mechanism list, only advertised mechanisms are eligible; an empty list means "try
anyway" (`ircv3.cpp:1024-1029`).

- **PLAIN** builds the `authzid\0authcid\0password` blob and base64-chunks it,
  wiping every intermediate (`ircv3.cpp:1067-1082`).
- **EXTERNAL** sends the base64 authzid, no password (`ircv3.cpp:1083-1089`).
- **SCRAM-SHA-256** is a three-stage exchange (`ircv3.cpp:1090-1098`) with hard
  caps on salt, nonce, field count, and iteration count
  (`kMaxScram*`, `ircv3.cpp:31-35`). Terminal success is **not** granted until
  the server signature is verified: `VerifyTerminalSuccess`
  (`ircv3.cpp:1055-1058`) requires `server_verified_` for SCRAM, so a server that
  omits the final signature fails closed rather than being trusted.

The destructor `SecureClear`s the config and every SCRAM intermediate
(`ircv3.cpp:1004-1013`), and `SecretsCleared()` (`ircv3.cpp:1426-1430`) lets a
caller assert nothing lingers.

### 1.5 Batches with bounded nesting

`HandleBatch` (`ircv3.cpp:2364-2410`) opens (`+ref`) and closes (`-ref`) batches.
Nesting is bounded two ways: a new child batch walks its ancestor chain and
aborts past `kMaxBatchDepth` (8) (`ircv3.cpp:2385-2389`), and open batches are
capped at `kMaxOpenBatches` (64) (`ircv3.cpp:2372`). Close-out-of-order (closing a
parent while a child is open) is rejected as a `batch-close-order` protocol error
(`ircv3.cpp:2397-2403`). Per-batch and global byte ceilings
(`kMaxBatchWireBytes`, `kMaxTotalBatchWireBytes`, `ircv3.cpp:39-40`) are enforced
both when a member arrives (`Process`, `ircv3.cpp:2745-2752`) and when a child
folds into its parent (`ircv3.cpp:2225-2234`). A batch that overflows is erased
whole, never truncated.

### 1.6 Outbound multiline builder — `PrepareMultiline`

`PrepareMultiline` (`ircv3.cpp:3002-3094`) is the only client-initiated `BATCH`
builder. It constructs the opening `BATCH +ref draft/multiline <target>`, one
child `PRIVMSG`/`NOTICE` per `MultilineLine`, and the closing `-ref`
(`ircv3.cpp:3042-3081`). What makes it load-bearing:

- It refuses to run unless both `batch` and `draft/multiline` are *enabled*
  (`ircv3.cpp:3005-3006`).
- Exactly one normalized target; a comma-list is rejected because the batch type
  defines a single target (`ircv3.cpp:3007-3010`).
- Effective limits come from `AdvertisedMultilineLimits` (`ircv3.cpp:2963-2986`),
  which parses the server's `max-bytes`/`max-lines` **but clamps them by the
  engine's own receive-side batch bounds** — an absent key leaves the receiver's
  bound in place, so the builder is never laxer than the reassembler and never
  unbounded (`MultilineLimits` struct, `ircv3.hpp:324-327`).
- `max-bytes` is counted over message *content* against a remaining allowance so
  the sum cannot overflow (`ircv3.cpp:3018-3032`).
- The prohibited cases — concat on the first line, concat on a blank line — are
  rejected rather than normalized away (`ircv3.cpp:3025-3028`; the struct comment
  at `ircv3.hpp:37-45`). A child that will not fit in 512 bytes is a **caller**
  error: the whole batch is rejected so the caller splits its own over-long lines
  into concat continuations, never a silent truncation (`ircv3.cpp:3067-3070`).
- Labeled-response, when enabled, scopes one `label` to the opening `BATCH` only
  (never the children) (`ircv3.cpp:3045-3053`); echo-message remembers the
  *reassembled* combined text so an echo is matched against the whole batch, not
  a single child (`ircv3.cpp:3087-3092`).

---

## 2. The capability-gating model

This is the single most important architectural idea in the subsystem, so it
gets its own section.

### 2.1 The catalog is two decisions, not one

`CapabilityCatalog()` (`ircv3.cpp:516-579`) is a static table of
`CapabilityDefinition{name, dependencies, auto_request}` (`ircv3.cpp:510-514`).
The `auto_request` bit is a **product-readiness** flag that is completely
independent of whether the engine can *parse* the capability. A capability can be
fully implemented on the wire — parsed, bounded, emitting typed events — and
still ship `auto_request = false`, so it is never requested by default.

The table makes this explicit in its comments: `batch` is default-off because "a
generic batch can unwrap a command the legacy model cannot safely consume"
(`ircv3.cpp:520-522`); `extended-join`, `multi-prefix`, `no-implicit-names`,
`userhost-in-names` are each default-off pending the specific legacy-adapter work
named inline (`ircv3.cpp:537-554`); `echo-message` is default-off because
content-only echo suppression is unsafe across bouncer sessions
(`ircv3.cpp:534-536`). Dependencies are also enforced — `SelectCapabilities`
skips any capability whose dependencies are not *available*
(`DependenciesAvailable`, `ircv3.cpp:1496-1503`), so `labeled-response` cannot be
requested without `batch`, `bot` without `message-tags`, etc.

`SelectCapabilities` (`ircv3.cpp:1514-1531`) is the concrete policy: for each
catalog entry it starts from `auto_request`, applies any per-frontend override,
skips if the request bit is false, skips if dependencies are unavailable, and for
`sasl` additionally requires a secure transport and usable credentials.

### 2.2 The per-frontend override lever

`SetCapabilityRequestEnabled(name, enabled)` (`ircv3.cpp:1382-1389`) writes into
`capability_request_overrides_` and is the **intended mechanism** for a frontend
to opt a catalogued capability in or out *before* `BeginRegistration`, once its
own product adapter is ready. The lever is fail-closed about what it accepts: it
rejects `cap-notify` (inseparable from CAP 302 state) and anything not in the
catalog (`ircv3.cpp:1386`) — STS, tags, commands, batch types, and ISUPPORT
tokens are never valid overrides (the contract is stated at `ircv3.hpp:271-276`).

### 2.3 The catalog default is GLOBAL; the lever is per-Engine

The catalog is a function-local `static` (`ircv3.cpp:518`) — one table shared by
the whole process. Both frontends (`v2.5-beta-1-modern` and the now-archived
`v1.0-pre-modern`) construct their **own** `Engine` instance, but they read the **same** catalog
defaults. So flipping an `auto_request` bit in `CapabilityCatalog()` changes the
default for *both* frontends at once. The override map, by contrast, lives on the
`Engine` instance (`capability_request_overrides_`, `ircv3.hpp:369`), so it is
per-frontend. This is why the override lever — not editing the catalog — is the
correct way for one frontend to enable a capability its adapter can handle while
the other frontend, whose adapter cannot, leaves it off. Editing the catalog is a
global act; the lever is a local one.

---

## 3. The legacy bridge

The bridge is where the modern wire meets Microsoft's 1998 MFC parser. It is
header-only and MFC-independent by construction — it takes the model lookup and
mutation as callables, so it never gains a dependency on MFC or on process-global
room state (`ircv3eventbridge.h:324-341`).

### 3.1 Flattening modern wire — `AdaptProtocolMessage` / `PrepareLegacyProtocolWire`

`AdaptProtocolMessage` (`ircv3eventbridge.h:593-624`) is the top of the inbound
adapter. For each parsed `Message` it produces an
`Ircv3LegacyMessageAdaptation` (`ircv3eventbridge.h:400-404`) with up to two
representations:

- **A typed `MessageContext`** carrying the *complete* parsed message (all tags,
  known and unknown) — built whenever the message has tags, is tag-only
  (`TAGMSG`), is an extended JOIN, or carried NAMES identities
  (`ircv3eventbridge.h:607-615`). UI consumers must reconstruct context from this,
  never from the tag-stripped legacy text (`ircv3eventbridge.h:22-24`).
- **Flattened legacy wire** via `PrepareLegacyProtocolWire`
  (`ircv3eventbridge.h:556-587`), *unless* the message is `TAGMSG`, which is
  tag-only metadata and must never reach the legacy unknown-command/history path
  even after a server strips every tag (`ircv3eventbridge.h:589-598, 616-622`).

`PrepareLegacyProtocolWire` does the shape surgery the old parser needs:
extended-JOIN's three params are flattened to the channel alone so the realname
cannot become `LookupDoc()`'s input (`ircv3eventbridge.h:563-570`); `353` is run
through `NormalizeLegacyNamesReply`; and a trailing `:` is re-inserted for the
handlers that dereference `args.back()` after only an `ASSERT`-level check
(`LegacyHandlerNeedsTrailingParameter`, `ircv3eventbridge.h:429-442, 579-585`).
The guard `HasSafeLegacyDispatchShape` (`ircv3eventbridge.h:411-427`) is the trust
boundary: it rejects malformed typed messages *before* flattening them, because
several release-build MFC handlers dereference mandatory args past an ASSERT-only
check — the adapter keeps Microsoft's valid-wire behavior without preserving its
null-deref trust boundary (`ircv3eventbridge.h:406-410`).

### 3.2 Applying typed events to the model — `ConsumeUserMutation` / `ConsumeNamesIdentities`

Identity mutations are applied on the UI thread against the document-owned
`CUserInfo`. `ConsumeUserMutation` (`ircv3eventbridge.h:327-341`) resolves the
target nick through a caller-supplied `find_user` callback, then invokes a
caller-supplied `apply_mutation` — resolution stays in the callback so the bridge
never touches MFC directly. `ClassifyUserMutation` (`ircv3eventbridge.h:289-322`)
is the pure part: it maps `Account`/`Away`/`HostChanged`/`RealnameChanged` events
to a flat `Ircv3UserMutation`, validating each field's length and rejecting
nicks that begin with a status prefix. ACCOUNT uses the event `source`; the SASL
900/901 numerics instead carry the affected nick in `target`
(`ircv3eventbridge.h:293-299`).

`ConsumeNamesIdentities` (`ircv3eventbridge.h:379-398`) is the many-at-once
version for a `353`: one reply names many members, so it returns *counts*
(`applied`/`unknown_user`/`ignored`, `ircv3eventbridge.h:151-155`) rather than a
single verdict. A member the receiving document does not already hold is
**counted, never created** — the `353` legacy wire alone decides membership
(`ircv3eventbridge.h:376-378, 390-393`).

### 3.3 NAMES/JOIN normalization — and the two divergent normalizers

There are **two** `NormalizeLegacyNamesReply` implementations, and they are not
equivalent. Contributors must not assume the frontends behave the same on a
`353`.

**v2.5 (`ircv3eventbridge.h:449-554`)** collapses multi-prefix down to the four
roles Microsoft's member model can represent — owner `.`, operator `@`, voice
`+`, spectator `>` — via `legacy_status` (`ircv3eventbridge.h:481-491`). It:

- strips every leading prefix symbol from each token and re-emits at most one
  (`ircv3eventbridge.h:504-512, 540`);
- splits `userhost-in-names` `nick!user@host` at `!`, keeps the bare nick for the
  legacy column, and carries the discarded `user`/`host` halves as flat
  `nickname/user/host` triples in `Event::context` for `ConsumeNamesIdentities`
  to apply as the same host mutation `chghost` uses
  (`ircv3eventbridge.h:343-374, 522-537`);
- treats an **empty `PREFIX=`** as legitimate (a network with no status
  prefixes): the strip loop becomes a no-op and nicknames pass through, rather
  than dropping the whole reply and leaving the member list empty
  (`ircv3eventbridge.h:460-479`);
- **rejects the whole reply** on any corrupt hostmask triple rather than
  half-populating identities (`ircv3eventbridge.h:344-350, 522-537`).

**v1 (`v1.0-pre-modern/transportadapter.h:307-356`)** is much lossier: it emits
`@` for any operator-ish role (`q`/`a`/`o`/`h`) and **nothing else** — no voice,
no owner, no spectator symbol survives (`transportadapter.h:330-347`). And its
`ParsePrefixMapping` (`transportadapter.h:250-270`) *rejects* an empty PREFIX
token (returns `invalid_line`), the opposite of v2.5's empty-PREFIX tolerance.
The v1 adapter also validates the whole legacy prefix fits Microsoft's
independent 50-byte nick/user/host buffers before recreating the wire, refusing
rather than silently aliasing identities (`LegacyPrefixFits`,
`transportadapter.h:168-190`).

Extended-JOIN is flattened to the channel alone in both frontends
(`ircv3eventbridge.h:563-570`; `transportadapter.h:370-377`).

**The v1 `LegacyJoinChannel` helper** (`transportadapter.h:209-219`) exists
because `no-implicit-names` must query the channel the server actually placed us
in, not the one we asked to join: a server-side forward (Libera `+f`, numeric
470) lands us elsewhere and echoes `JOIN <other>`, so the channel is recovered
from the JOIN's own params (accepting both `JOIN #chan` and trailing `:#chan`)
and then re-validated by `PrepareExplicitNamesRequest`
(`transportadapter.h:202-241`).

### 3.4 Bounded UI hand-off

`DrainIrcv3EventsForUi` (`ircv3eventbridge.h:643-681`) bounds one UI-thread turn:
it pulls events in batches (`kIrcv3UiDrainBatch` 128, capped at
`kIrcv3UiDrainMaximum` 512), invokes the sink per event, and — crucially —
catches exceptions per-event so one faulty view cannot block later typed events
in the same turn, and treats a poll allocation failure as a rejected hand-off
rather than unwinding across the window proc (`ircv3eventbridge.h:657-673`). The
`IrcTransportIngressGate` (`ircv3eventbridge.h:45-100`) and the v1 `SessionGate`
(`transportadapter.h:431-495`) are the generation/phase validators that stop a
queued event from a previous server session reaching the stateful MFC parser.

---

## 4. Testing model and the MSVC boundary

This boundary is load-bearing — internalize it before you claim a change is
"tested."

**What runs on Linux (via meson):**

- `portable/tests/ircv3_test.cpp` — the standalone, MFC-independent engine test,
  built as `comicchat-ircv3-tests` and registered as `comicchat-ircv3`
  (`portable/meson.build:426-432`). This exercises the engine directly: parsing,
  CAP negotiation, SASL, batches, multiline, flood.
- `v2.5-beta-1-modern/tests/transport_ui_bridge_test.cpp` — the bridge test,
  built against a **fake model** (no MFC), registered as
  `comicchat-transport-ui-bridge` (`portable/meson.build:480-486`). It exercises
  the header-only bridge — `AdaptProtocolMessage`, the NAMES normalizers,
  `ConsumeUserMutation`/`ConsumeNamesIdentities` — against an MFC-independent
  fake, because the bridge takes its model as callables.
- `v1.0-pre-modern/tests/transport_adapter_test.cpp` — the v1 adapter test
  (`portable/meson.build:488-493`).

**What does NOT run on Linux:** the real MFC consumers — `chatview.cpp`,
`ircsock.cpp`, `userinfo.cpp`, and the rest of the `v2.5-beta-1-modern`/
`v1.0-pre-modern` frontends — include MFC's `stdafx.h` and compile **only under
MSVC CI**. The meson file is explicit that the UI-contract tests are kept as
standalone executables specifically so they do not drag MFC (or a second
`main()`/`WinMain`) into the portable build, and it guards the MFC-adjacent
targets behind `host_machine.system() != 'windows'`
(`portable/meson.build:463-478`).

The consequence for contributors: a Linux `meson test` run can prove the engine
and the bridge *logic* are correct against a fake model, but it **cannot** prove
the actual MFC consumer wires that logic into visible behavior. The wiring
between the bridge and `CUserInfo`/the chat view is verifiable only by the MSVC
CI gate. Treat a green Linux suite as necessary, not sufficient.

---

## What is NOT done

Per-capability status (implemented / partial / observe-only / unsupported) is the
job of [`IRCv3-COVERAGE.md`](IRCv3-COVERAGE.md) — consult it, not this section,
for whether a given capability is finished. The subsystem-level gaps a
contributor should know up front:

1. **Typed events with no legacy consumer.** The engine emits ~28 `EventType`
   variants (`ircv3.hpp:81-110`), but the v2.5 bridge only classifies a handful
   into model mutations: `Account`, `Away`, `HostChanged`, `RealnameChanged`
   (`ClassifyUserMutation`, `ircv3eventbridge.h:289-322`), `ChannelRenamed`
   (`ClassifyChannelRename`, `ircv3eventbridge.h:219-227`), `StandardReply`
   (`ClassifyStandardReply`, `ircv3eventbridge.h:262-287`), and the `353`
   `MessageContext` (`ClassifyNamesIdentities`, `ircv3eventbridge.h:351-374`).
   The rest — `Typing`, `Reaction`, `Netsplit`, `Netjoin`, `Metadata`,
   `Redaction`, the `Monitor*` family, `ReadMarker`, and so on — are emitted,
   bounded, and retained, but **no descendant window turns them into UI**. The
   ledger's observe-only rows and its `TAGMSG` note
   (`IRCv3-COVERAGE.md:198-206`) are the authoritative list; the code truth is
   simply that the bridge's typed-event surface stops at identity/status
   mutations plus NAMES.
2. **MFC consumers unverifiable on Linux.** As above — the real
   consumers compile only under MSVC CI, so any "the UI now shows X" claim is
   unproven by the portable test suite. Several coverage rows explicitly stay
   `partial` for exactly this reason (e.g. `no-implicit-names`,
   `userhost-in-names`, `draft/channel-rename` — see the CI-only notes at
   `IRCv3-COVERAGE.md:147-151`).
3. **Capabilities default-off.** Many fully-parsed capabilities ship
   `auto_request = false` in the catalog (`ircv3.cpp:520-576`) and require a
   frontend to call `SetCapabilityRequestEnabled` after shipping its adapter.
   Flipping a catalog default is a **global** change affecting both frontends
   (see §2.3); do not flip one to "turn on" a feature for a single frontend —
   use the override lever.

### A drift to watch (code vs. ledger)

The coverage ledger's `file:line` anchors into `ircv3.cpp` have drifted from the
current source. For example the ledger cites the batch engine at
`ircv3.cpp:1989-2172` and multiline at `:2980-3070`
(`IRCv3-COVERAGE.md:133,158`), but at the time of writing `HandleBatch` is at
`ircv3.cpp:2364`, `AdvertisedMultilineLimits` at `:2963`, and `PrepareMultiline`
at `:3002`. This is the reason for the warning at the top of this document:
**read the code, not the ledger, for structural truth** — the ledger is a status
snapshot and its line numbers lag the actively-edited engine. When you touch this
file, prefer citing symbols over line ranges.
