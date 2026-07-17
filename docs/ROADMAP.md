# Comic Chat: Reinked — Roadmap

This roadmap enumerates the remaining work to complete the IRCv3 protocol layer
and the client UI, and the order in which to do it. It is grounded in the tree
at the time of writing and cross-references [`IRCv3-COVERAGE.md`](IRCv3-COVERAGE.md)
(the authoritative per-capability ledger) and [`ircv3-internals.md`](ircv3-internals.md).

## Guiding constraints

These are load-bearing; every item below is filed against them.

- **Verifiability is the gate.** The portable engine, bridge adapters, and
  headless renderer are testable on Linux via `meson test -C portable/build`
  (currently 11/11). The MFC clients (`v*-modern/`, e.g. `chatview.cpp`,
  `panel.cpp`) compile **only under MSVC CI** — code written for them cannot be
  proven on this host and must be marked unverified until CI runs.
- **Wire support ≠ product readiness.** Capabilities are default-**off** in
  `CapabilityCatalog()` by design. "Implemented" means the parser/state/adapter
  path plus causal tests exist; enabling a capability is a separate decision and
  several are blocked on product policy, not code.
- **No false completion.** A typed event with no consumer is not support.
  "Done" requires a causal test where behavior is observable, or an explicit
  unverified-pending-CI marker where it is not.
- **Provenance & safety.** Do not modify the Microsoft snapshots (now in
  `version/*` branches). Deploy and public push are human-gated.

Status legend: ✅ done · 🟡 partial · ⬜ not started · 🔒 MSVC-CI-only ·
⚖️ needs product decision.

## Already shipped (context)

- ✅ Fleet integration onto `main`; branch/worktree cleanup; `version/*` archival
  branches; build assets relocated to `portable/assets/`.
- ✅ IRCv3: outbound multiline `BATCH` builder; NAMES hostmask delivery
  (userhost-in-names); three membership defects fixed (extended-join member
  drop, empty `PREFIX=` drop, v1 no-implicit-names channel source).
- ✅ Coverage ledger refreshed against the tree; `ircv3-internals.md` written;
  docs drift audit.
- ✅ Link-time optimization + aggressive codegen flags (Meson verified; MSVC
  LTCG set pending CI).

## Phase 1 — Verifiable IRCv3 engine/bridge completion (Linux/meson)

Every item here has a Linux test as its gate. This is the honest "finish it"
surface. Serial on `portable/src/net/ircv3.cpp` (one hot file — do not
parallelize edits to it).

| # | Item | Anchor | Gate | Status |
|---|------|--------|------|--------|
| 1.1 | Multiline `FAIL BATCH MULTILINE_MAX_BYTES/MAX_LINES/INVALID_TARGET/INVALID` on violation | `ircv3.cpp` `PrepareMultiline`/`HandleBatch` | `ircv3_test.cpp` cases per RFC | ⬜ |
| 1.2 | Wire `RecoveryCommands()` into a production reconnect caller (no live caller today) | `ircv3.cpp:1468`, client reconnect path | causal test that reconnect emits bounded recovery | ⬜ |
| 1.3 | Labeled-response full presentation (beyond terminal ACK) | `ircv3.cpp` label path | correlation/presentation test | 🟡 |
| 1.4 | Typing typed-event → `Ircv3UserMutation` bridge classification (testable half; MFC icon 🔒) | `ircv3eventbridge.h` `ConsumeUserMutation` | bridge fake-model test | ⬜ |
| 1.5 | Reaction → status-line presentation (testable half; in-comic needs 1.6-style msgid index) | `ircv3eventbridge.h` | bridge fake-model test | ⬜ |
| 1.6 | Metadata whitelist → document-owned `CUserInfo` field (testable half) | `ircv3eventbridge.h` | bridge fake-model test | ⬜ |

## Phase 2 — Portable renderer completion (Linux headless)

The renderer is a title-panel foundation; these are the real "finish the UI"
items that are verifiable headlessly (`SDL_VIDEODRIVER=dummy … --png`), gated by
`source_raster_test` and render goldens.

| # | Item | Anchor | Status |
|---|------|--------|--------|
| 2.1 | Comic balloon geometry, tails, thought/whisper shapes | `portable/src/render`, `source_raster.*` | ⬜ |
| 2.2 | `CBodyUnary` avatar pose/body compositing from AVB | render + asset decode | ⬜ |
| 2.3 | Expert placement (avatar order, panel splitting) port | layout | ⬜ |
| 2.4 | `CLabel` font-bounding-box exactness | text measurement | 🟡 |
| 2.5 | Full room/page/composer/member-list shell | app/frontend | ⬜ |

## Phase 3 — MFC view consumers (MSVC-CI-only) 🔒

Writable here, verifiable only under MSVC CI. Each lands with an unverified
marker and a bridge-side fake-model test for the logic that *can* run on Linux.

| # | Item | Anchor | Status |
|---|------|--------|--------|
| 3.1 | Redaction as **mark** (`[redacted]` swap + re-layout); requires plumbing a msgid→balloon index (build the index as a portable, Linux-testable structure) | `panel.cpp`, `chatdoc.cpp`, portable index | ⬜ 🔒 |
| 3.2 | Read-marker inbound display (own marker from bouncer) | `chatview.cpp` | ⬜ 🔒 ⚖️ |
| 3.3 | Typing indicator icon (member-list state overlay, not the flag ladder) | `memblst.cpp`, `chat.rc` | ⬜ 🔒 |
| 3.4 | Reaction in-comic surface | `panel.cpp` | ⬜ 🔒 |

## Phase 4 — Product-policy decisions (needs human) ⚖️

Not coding tasks. Each blocks enabling a default-off capability.

- 4.1 Outbound typing: broadcast keystroke-granularity presence? (there is no
  outbound builder today — deciding "yes" creates a consent surface).
- 4.2 Read markers: disclose read state to the network, or inbound-only?
- 4.3 Redaction: display convention or guarantee? (mark-in-place leaves text in
  memory and in the `.ccc` file — decide before shipping 3.1).
- 4.4 Which membership caps to flip on: `extended-join`, `multi-prefix`,
  `no-implicit-names`, `userhost-in-names` now meet their catalog preconditions;
  flipping requires the per-frontend `SetCapabilityRequestEnabled` lever wired
  (no production caller today) and a policy sign-off.

## Phase 5 — Release (human-gated)

- 5.1 MSVC CI green on the LTCG flags and any Phase 3 MFC work.
- 5.2 Reproducible signed unofficial build; provenance/NOTICE intact.
- 5.3 Public push / release only on explicit human go.

## Execution order

1 → 2 in parallel tracks (different files); 3 after its Phase-1/2 dependencies;
4 is a standing gate consulted before any capability flip; 5 last. Within
Phase 1, strictly serial on `ircv3.cpp`. Update this file and the ledger as each
item lands.
