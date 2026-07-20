# Microsoft Comic Chat wire audit

**Audit date:** 2026-07-20
**Behavioral reference:** Microsoft Comic Chat source commit
`c7df00f60bc8e9fdef413f139e61f7c37e024684`

This is the source-level compatibility ledger for the portable Zig client. It
separates exact live wire behavior from modern additions and from retired
Windows integrations. The reference checkout was inspected in full for the
IRC-facing paths; the large `irc.txt`, `ircnew.txt`, and `ircorig.txt` files are
captured sessions rather than normative protocol definitions.

## Checked source surfaces

- `ircproto.cpp` / `ircproto.h`: message serialization, JOIN, CREATE, KICK,
  MODE, INVITE, TOPIC, IRCX probes, and CTCP constants.
- `ircsock.cpp`: receive dispatch, numeric 800's two-state IRCX negotiation,
  DATA routing, JOIN/CREATE completion, and server numerics.
- `protsupp.cpp`: UDI construction and parsing, actions, sounds, away state,
  avatar/profile/backdrop comments, whispers, and CTCP information controls.
- `chat.rc` / `resource.h`: Enter Room, Create Room, Kick, Ban, Invite, Sound,
  Away, profile, and room-property field contracts and source dimensions.
- `histent.cpp`, `chatdoc.cpp`, `avbfile.*`, and the source-parity fixtures:
  saved records and the authored rendering state that UDI selects.
- `profile.txt`, `readme.txt`, `rtwsupport.txt`, and the source README:
  defaults, documented user behavior, and platform-only integrations.

## Wire compatibility matrix

| Surface | Microsoft source behavior | Portable status |
| --- | --- | --- |
| IRCX discovery | Send `MODE ISIRCX` immediately after connect; numeric `800 ... 0 ...` triggers `IRCX`; only `800 ... 1 ...` means enabled (`ircsock.cpp:1048-1053`, `2817-2895`). | Exact state machine. The probe precedes modern CAP/NICK/USER; DATA is disabled until state 1. Ordinary IRC remains the fallback. |
| Registration | Source sends NICK/USER after discovery; it predates IRCv3 CAP/SASL. | Source ordering is preserved around a modern CAP/SASL layer. Exact initial bytes and both numeric 800 states have tests. |
| Trailing parameters | `PRIVMSG`, `NOTICE`, `DATA`, `AWAY`, `KICK`, and `TOPIC` use explicit `:` trailing fields, including one-word or empty values. | Exact. The generic serializer has an explicit `force_trailing` contract and byte tests. |
| Comic UDI | `#G...E...[R]M...[T...]`; non-IRCX embeds `(#...) ` in PRIVMSG, IRCX sends `DATA target CCUDI1 :#...` then readable text (`bInsertAnnotations`, `bChatSendToTarget`). | Exact live send/receive. Action uses raw readable text plus `M5`; selected and whisper recipients populate `T`. |
| Say/think/action/whisper | Comic action is selected by UDI; text fallback wraps CTCP ACTION. Private messages force whisper state. | Exact for the five portable say modes, including private target routing and sender-prefix action display. |
| Sound | `PRIVMSG target :\x01SOUND <quoted-file> <message>\x01`; no UDI (`bChatSendSound`). | Exact. CTCP filename quoting/unquoting, public/private targeting, dialog fields, and action-box display are tested. |
| Avatar comments | `# Appears as <character>[.<url>]`; IRCX uses DATA, IRC uses PRIVMSG. `# GetCharInfo` requests a refresh. | Exact for bundled characters. Remote avatar download is deliberately disabled; no retired Microsoft art-server dependency is restored. |
| Profile comments | `# GetInfo` / `# HeresInfo: ...`; empty profile uses `profile.txt`. | Exact control bytes and exact source default. The portable build does not publish email, a homepage, or a personal name automatically. |
| Backdrop comments | `# BDrop2: name,url` followed by legacy `# BDrop:  base`. | Exact send/parser compatibility. Applying a received remote backdrop to the room renderer remains partial. |
| Enter Room | `JOIN <room>` or `JOIN <room> <password>` (`ircproto.cpp:806-820`, `chat.rc:830-842`). | Exact. The dialog now exposes its source password field and passes it to JOIN. |
| Create Room | `CREATE <room> [creation-modes] [limit] [password]`, then source completion applies topic/modes (`ircproto.cpp:822-848`, `protsupp.cpp:4142-4201`). | Live. The dialog exposes room, topic, modes, limit, and password; CREATE uses source parameter order and TOPIC follows in queue order. Boolean mode checkboxes are represented by one modern mode token. |
| Kick/ban/invite | Optional ban first, then `KICK room nick :reason`; ban uses `MODE room +b mask`; invite uses `INVITE nick room`. | Exact command order and bytes. Kick exposes reason and optional ban mask. |
| Away | Send `AWAY :message` (or bare `AWAY`), then `\x01AWAY [message]\x01` to every joined room (`ircproto.cpp:912-939`, `protsupp.cpp:3744-3769`). | Exact. Peer controls are consumed, update roster away state, and never become comic bubbles. |
| CTCP information | VERSION, PING, TIME, EMAIL, URL, and CLIENTINFO requests receive private NOTICE replies; X-VCHAT is ignored. | Live. VERSION/PING/TIME/CLIENTINFO reply without personal data; EMAIL/URL are deliberately empty to preserve the release privacy boundary. |
| Same account + same nick | This is an Onyx session extension, not a Microsoft IRCX behavior. | Supported through SASL plus exact `SESSION RESUME` credential attachment. Separate client processes share one logical nick/roster identity after resume; the server retains all attached transports. |
| IRCX PROP/ACCESS/LISTX/admin | Source has room-property and search workflows beyond comic payload transport. | Protocol substrate is present but not all application workflows are reachable. This remains explicitly partial in the completeness audit. |
| DCC / NetMeeting | Source hands these to Windows file-transfer and NetMeeting consent/platform code. | DCC codec/send substrate exists, but inbound ownership/consent UI is incomplete. NetMeeting is intentionally retired. Neither is advertised as a finished portable feature. |

## Interoperability invariants

1. A server advertisement such as `005 COMICCHAT=DATA` does not enable DATA;
   only numeric 800 state 1 does.
2. Comment controls use the same bytes on both transports. Only the outer
   command changes between IRCX DATA and ordinary PRIVMSG.
3. UDI and readable text remain two messages on IRCX and one embedded message
   on plain IRC. A pending UDI is paired by target and logical nick.
4. Multiple Onyx attachments using the same account and nick intentionally
   collapse to one logical roster member; they are not duplicate participants.
5. CTCP SOUND and AWAY are controls, not speech. They are rendered or consumed
   according to the source before ordinary comic-line insertion.
6. No certificate, private key, personal name, email address, or homepage is
   placed on the wire by a peer probe or included as a release default.

## Known non-wire gaps

The remaining gaps do not change the correctness of the live comic messages:
native file-pickers and inbound DCC consent, full IRCX property/search/admin
workflows, persistent automation/notifications/profile editing, received
backdrop application, and retired NetMeeting. They stay visible in
`PORTABLE_COMPLETENESS_AUDIT.md`; this document does not relabel them complete.
