# Portable ComicChat Completeness Audit

**Date:** 2026-07-20
**Scope:** the complete repository, with product reachability evaluated from
the portable executable rather than inferred from module or test existence.

## Verdict

The repository contains a portable rendering, protocol, transport and
native-window foundation. The historical Microsoft source is an external,
pinned behavioral reference; it is not vendored. The portable desktop application now has an
interactive Microsoft-shaped shell, unified pointer input, multi-room chat,
Unicode editing, and an exhaustive dialog registry. It is not yet feature
complete: clipboard/IME/accessibility, native file pickers, complete typed
behavior for every legacy dialog, DCC transfer ownership, and persistent
automation/notification editors remain.

This distinction is important:

- **Reachable** means a user can exercise the feature through `comicchat app`
  or a documented CLI subcommand.
- **Substrate** means a tested library implementation exists but the desktop
  application has no workflow that invokes it.
- **Partial** means the application exposes only part of the source contract.
- **Missing** means no portable implementation was found.

## Repository and build coverage

- The historical upstream checkout was previously verified against its pinned
  revision before this repository was migrated to the portable-only layout.
- The portable tree contains 92 files under `src/`. `src/root.zig:68-118`
  explicitly references every portable module and the four source-parity test
  modules so their inline tests are compiled and run.
- The current release test gate reports 406 passed tests and one intentionally
  skipped platform-conditional test.
- Native Linux plus x86_64 Windows, FreeBSD, and OpenBSD release builds pass.
- The published source archive includes the source tree and the crypto,
  protocol, and certificate-loader subset exported from the exact pinned Onyx
  revision. Binary archives contain only the executable, current documentation,
  licenses, and third-party notices.

## Product completeness matrix

| Area | State | Evidence and boundary |
|---|---|---|
| Repository shape | Portable-first | The live tree contains the Zig product, runtime assets, documentation, and tooling; the retired MFC/C++ source is external-only. |
| Source comic rendering | Reachable | Original page, title, layout, balloon, figure and raster modules feed the shared strip path; CLI render commands and the app use them. Golden/source-parity tests cover the rendering pipeline. |
| AVB/BGB content | Reachable | Authored character/backdrop decoding, icons, masks and figure composition are used by the renderer and UI. Program chrome uses modern glyphs; product art remains original. |
| Shell geometry | Reachable | The modern seven-menu/condensed-toolbar chrome retains the 29px tabs, 80/20 main split, 30/70 comic side split, 23px composer and status panes through shared geometry. |
| Comic/text buffers | Partial | Both modes, bounded history paging, visible scroll position, wheel input, per-room transcripts/drafts, modal atomic `.ccc` open/save, and modal PNG export are reachable. Selection/copy, page-break editing, native pickers, and printing remain. |
| Body camera | Partial | Redrawn high-resolution dial faces use the source wheel thresholds. Pointer drag and keyboard arrows/Home drive a visible intensity puck; the context menu exposes Freeze, Character, and Neutral. Double-click and an immediate Send Expression command remain. |
| Member list | Reachable core | NAMES/JOIN/PART/QUIT/NICK update the live roster; active counts exclude departed history. Icon/list modes, bounded wheel scrolling, retained viewport position, keyboard reveal, selection, character preview, and right-click actions are reachable. Dynamic role-badge artwork remains incomplete. |
| Menus/toolbars/buttons | Partial | The condensed modern toolbar is backed by complete popups for File, Edit, View, Format, Room, Member, and More. Checked view state, stable hit testing, context menus, and command-specific dialogs are live; command enablement remains incomplete. |
| Composer | Partial | UTF-8 scalar insertion, codepoint-safe movement/delete, selection, mouse caret placement, horizontal caret tracking, per-room drafts, bounded copy/cut/paste, undo/redo, and the 400-byte wire bound are live. Native clipboard, multiline input, IME, and formatting controls remain. |
| Live IRC/IRCX comic chat | Reachable core | Connect/register/reconnect, multi-room JOIN/PART, per-room roster/transcript routing, IRCX `DATA CCUDI1`, embedded UDI, avatar announcements and all five say modes are wired. |
| TLS, proxies, CAP, SASL, STS | Reachable | The live client composes verified TLS, proxy connection, IRCv3 negotiation, SASL and persisted STS. Credential input is file-based and refused over plaintext. |
| Resolver and sockets | Reachable | The Onyx DNS wire codec and resolv.conf policy drive hostname lookup. Pure-Zig native socket adapters cover Unix and direct Winsock; Wine falls back to the Windows DNS service only when direct UDP resolution is unavailable. No C transport or resolver source is shipped. |
| Onyx reusable sessions | Reachable | TOKEN/MTOKEN parsing, host/account-scoped persistence, expiry and resume preference are implemented in `src/net/session_store.zig:19-187` and connected through `ConnectionRuntime`. This supports separate same-account/same-nick clients when the server honors `SESSION RESUME`. |
| Modern IRC feature state | Reachable internally, limited UI | `net/client.zig` owns `ircv3.Session` and `features.State`; CAP and negotiated state are live. Most identity, metadata, batch, read-marker, standard-reply and moderation state has no visible UI. |
| Profiles/backdrops | Partial | Character and backdrop dialogs enumerate every bundled asset, render live previews, and invoke the live avatar/backdrop helpers. The complete persistent profile browser remains. |
| DCC transfer | Substrate | Offer codec and transfer loops exist; `Client.offerFile` sends an offer (`src/net/client.zig:493-501`). No receive dispatch, consent dialog, file picker, progress, cancellation or safe destination workflow exists. |
| IRCX key strings | Substrate | Parsing, mutation and diffing are implemented in `src/proto/keystring.zig`; no live PROP query/send application path uses them. |
| Automation rules | Substrate | Source event/action model, matching, substitution, flood control and snapshot diffing exist in `src/comic/rules.zig`; there is no live event dispatcher, action executor, editor or persistence. |
| Notifications | Substrate | Mask construction and WHO snapshot folding exist in `src/comic/notify.zig`; there is no polling owner, notification UI or persistence. |
| `.ccc` / `.ccr` | Partial | Bounded codecs exist; `.ccc` open/save and PNG export are live, atomic, and reachable from dedicated File dialogs. Locator application, recent files, and native pickers remain. |
| Multiple rooms/windows | Reachable core | Up to 64 case-insensitive room tabs own independent transcript, roster, draft, joined, and unread state. `/join`, `/switch`, `/part`, and clickable tabs are live; favorites and separate child windows remain. |
| Microsoft dialogs | Partial | All 40 templates plus three portable file dialogs have typed IDs, source dimensions, modal routing, hover/focus/validation states, selection, mouse caret placement, keyboard editing, and shared controls. Room, appearance, identity, away, moderation, invite, whisper, open/save, and export acceptance is wired; remaining template-specific models are incomplete. |
| Pointer/touch | Partial | X11, Wayland, and Win32 emit shared motion/button/wheel events and activate stable targets. Touch gestures are not implemented. |
| Clipboard/IME/accessibility | Partial | The shared event contract carries modifiers; the app owns UTF-8-safe clipboard/selection state and a stable semantic UI snapshot. Native clipboard, compose/IME, AT-SPI, and UIA bridges remain. |
| DPI scaling | Partial | Geometry resizes proportionally inside the framebuffer, but Wayland stays at scale 1 and Win32 is system-DPI aware rather than per-monitor-v2. |
| Printing | Missing/decision required | No portable print or print-preview backend. PNG/PPM diagnostic output exists, but no desktop export workflow invokes it. |

## Source UI contract status

The implemented shell follows the Microsoft menu order and major geometry,
including its chat buffer composition and body camera. Modernization is limited
to application chrome: toolbar and say-window program icons/buttons are modern;
characters, backdrops, panels and emotion-face art remain authored source art.

The portable UI must not be called complete until each of these source-facing
contracts is either reachable or deliberately classified obsolete:

1. Modifiers, clipboard, IME, output-scale, and accessibility events in the
   unified native contract.
2. Scrollbars, text selection, popup menus, and complete keyboard roving.
3. Retained ownership for transfers, favorites, automation, notifications, and
   persistent preferences.
4. Complete typed behavior for all 40 dialog contracts, including connection,
   moderation, backdrop, personal profile, fonts/colors, notifications, rules,
   and transfer.
5. `.ccr` application, recent files, native file selection, and an explicit print decision.
6. Clipboard, Unicode/IME, DPI, accessibility and safe native file selection.

## Verification performed

```text
PASS  zig fmt --check build.zig source_ui_assets.zig src
PASS  zig build test --summary all
PASS  zig build --summary all
PASS  zig build -Dtarget=x86_64-windows -Doptimize=ReleaseSafe
PASS  zig build -Dtarget=x86_64-linux -Doptimize=ReleaseSafe
PASS  zig build -Dtarget=x86_64-freebsd -Doptimize=ReleaseSafe
PASS  zig build -Dtarget=x86_64-openbsd -Doptimize=ReleaseSafe
PASS  git diff --check
PASS  portable-only tree contains no retired MFC/C++ source directory
PASS  record-codec demo, 650x1655 strip PPM, and 315x315 backdrop PNG
PASS  all 43 dialogs at 640x480, 800x600, and 960x720 without field/action overlap
PASS  every compact menu popup and command row remains on-screen and clickable
PASS  Xvfb X11 menu, Settings input/Tab, Escape, and composer event smoke
PASS  headless Wine live TLS connect, status dialog, invalid-port recovery,
      reconnect, member scroll/select/context menu, top menu, and composer input
PASS  six Linux/Windows deterministic UI surfaces are byte-identical
PASS  release archive checksums verified for all four binary packages and the
      source archive containing the required pinned Onyx TLS source subset
```

Not claimed by this audit: a live `eshmaki.me` SASL EXTERNAL login or a live
same-nickname multi-client resume check with this transport revision, a
physical Windows desktop session, or real-compositor Wayland clipboard/scale
behavior. Those require their respective environment or the missing feature
implementation rather than more unit tests.
