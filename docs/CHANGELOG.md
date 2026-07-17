# Changelog

All notable changes to this fork are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/); capability status for IRCv3 is
tracked separately in [docs/IRCv3-COVERAGE.md](IRCv3-COVERAGE.md).

Note on scope: the newest work is an IRCv3 hardening pass — a send-side
multiline builder, NAMES hostmask delivery, and three membership-loss fixes —
layered on top of a large multi-branch integration merge. It is hardening plus a
builder plus documentation, not "IRCv3 complete." Every new or extended IRCv3
capability remains **default-off**: implemented is not the same as enabled, and
none of these changes negotiates a capability on the wire by itself.

## [Unreleased] — 2026-07-17

### Added

- Outbound multiline `BATCH` construction: the engine could previously only
  reassemble inbound multiline batches, never build one. `PrepareMultiline` now
  opens `BATCH +ref draft/multiline <target>`, tags continuation children with
  `draft/multiline-concat`, and closes with `BATCH -ref` — gated on the
  capability, one target only, advertised max-lines/max-bytes clamped to the
  engine's own bounds so an absent or over-large advertisement is never trusted,
  over-long lines rejected rather than truncated. `draft/multiline` stays
  default-off. (5135655)
- NAMES hostmasks delivered to the legacy user model: with `userhost-in-names`
  the 353 normalizer had previously found the `!` only to discard the hostmask,
  leaving the capability's own precondition unmet. The hostmask is now split in
  the normalizer and carried as flat identity triples through the existing event
  context, reusing the chghost host-mutation path so `MatchesNickMask` gets real
  data. Nickname safety and fail-closed rejection are preserved; the capability
  stays default-off. (8fc4474)

### Fixed

- An extended-join member is now delivered even when the identity-state map is
  at its bound: previously `HandleJoinMessage` rejected the whole JOIN at
  `kMaxStateEntries`, so a well-formed member vanished permanently to protect an
  optional account/realname annotation. At the bound the JOIN is now returned
  unconsumed — the member is delivered, only the annotation is dropped, and
  neither map is partially written. (ba7cc68)
- An empty `PREFIX=` ISUPPORT token no longer discards the whole NAMES reply:
  empty is a legitimate token meaning the network has no status prefixes, but
  the normalizer required a leading `(` and returned nothing, leaving the member
  list permanently empty on such a network. Empty is now treated as an empty
  prefix set — nicknames pass through and the hostmask split still runs, while
  malformed prefix tokens still fail closed. (4003632)
- v1's no-implicit-names NAMES request now queries the channel actually joined
  rather than the one requested: the self-JOIN handler built its explicit NAMES
  from `GetMyChannel()`, so a server-side forward (e.g. Libera `+f`, numeric 470)
  had it query a room the client was not in, and an empty configured channel
  dropped the connection. A pure, unit-tested `LegacyJoinChannel` helper now
  recovers the channel from the JOIN's own parameters (both `JOIN #chan` and
  trailing `:#chan` forms). (0aba79d)

### Changed

- Integrated the multi-branch fleet: merged the `agent/*`, `codex/*`, and
  `claude/*` work into `integration/all` (the sanitizer-release-gate merge at the
  base of this pass), bringing the portable engine, the v1 and v2.5 modern
  frontends, and the assurance/test tooling to a common baseline that the IRCv3
  hardening above builds on. (1adea2d)

### Documentation

- Refreshed the IRCv3 coverage ledger against the tree after it had drifted 27
  commits stale and wrongly marked implemented capabilities as unsupported:
  extended-join, multi-prefix, no-implicit-names, and userhost-in-names moved to
  partial with file:line evidence (none to implemented), and the STS
  self-contradiction over the Unix/BSD production caller was resolved. All five
  capabilities remain default-off — normalization is not negotiation. (c047b56)
- Recorded the multiline builder and NAMES hostmask delivery as landed (the
  ledger had listed them as open gaps), and logged three defects found by audit
  rather than test failure — the extended-join member-drop, v1's
  `GetMyChannel()` NAMES source, and the empty-`PREFIX=` 353 drop — each of which
  silently costs membership and blocks its capability's flip. (35684bb)
- Marked those three audited NAMES/JOIN defects as fixed with causal tests, noted
  that PART pruning was deliberately not added (the engine tracks only its own
  channels, so pruning on PART would drop identity for a user still visible
  elsewhere), and that the v1 call-site wiring stays MFC/CI-only. All five
  capabilities stay default-off pending the per-frontend
  `SetCapabilityRequestEnabled` lever, which still has no production caller. (0f1f9f8)
