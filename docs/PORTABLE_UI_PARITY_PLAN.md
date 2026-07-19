# Portable ComicChat UI Parity Plan

**Status:** source-geometry shell implemented; interaction foundation active  
**Date:** 2026-07-19  
**Scope:** `/home/kain/comicchat/src/`; the historical Comic Chat repository
at the pinned upstream revision is an external behavior reference.

## Outcome

Turn the portable framebuffer client into a complete native ComicChat desktop
application on Wayland, X11 and Win32 without forking the shared comic renderer
or weakening Onyx/TLS/session behavior.

The portable client preserves the Microsoft Chat 2.5 user-visible structure,
geometry, command model and chat-buffer behavior. Modernization is limited to
presentation and platform integration: DPI scaling, a cleaner Microsoft color
system, Unicode input, accessibility, safe persistence, and native
Wayland/X11/Win32 plumbing. It is not permission to redesign the product.

## Fixed UI contract

The released source is the geometry authority:

- retain the File/Edit/View/Format/Room/Member/Favorites/Window/Help menu order;
- retain menu, toolbar, 29-pixel room-tab bar, chat buffer, say window and
  two-pane status bar in the original vertical order;
- retain the 80/20 conversation/member split from `CSplitChatV`;
- retain the comic-mode 30/70 member/body-camera split from `CSplitChat`;
- retain the 23-pixel minimum say window and five 24-pixel Say/Think/Whisper/
  Action/Sound controls from `CSayWnd`;
- retain comic/text buffer switching, room tabs, member-list/body-camera
  behavior, Page Up/Page Down history forwarding and composer focus routing;
- scale these measurements together at non-96-DPI output instead of changing
  their proportions.

The visual skin may use current Microsoft-neutral colors, modern program
icons, clearer focus, high-contrast roles and modern text rendering without
moving or replacing the source controls. Character, backdrop, comic and
emotion-face artwork remains source-authored product content.

## Evidence boundary

- Rendering/layout behavior: the pinned upstream source and `src/comic/source_*_test.zig`.
- Original menus/dialogs: the upstream `chat.rc` and their owning MFC classes.
- Portable protocol/network behavior: `src/net/`, `src/proto/`, and
  `docs/PROTOCOL.md`.
- Portable UI today: `src/client/`, `src/main.zig`, and `src/platform/`.
- Workflow: `/home/kain/CLAUDE_CODEX_WORKFLOW.md`.
- Research: `/home/kain/research/INDEX.md`, especially UI architecture,
  accessibility, deterministic testing, IRCX/IRCv3 and Windows architecture.

## Current substrate to preserve

- Source-derived comic page, title, avatar, pose, mask, backdrop and balloon
  rendering.
- Shared framebuffer presentation on X11, Wayland and Win32.
- Nonblocking Onyx/TLS/IRC lifecycle, SASL, STS and reusable sessions.
- Same-account/same-nick Onyx session resume.
- IRCX/IRCv3 state, UDI, profile/backdrop controls, DCC codec/transfer,
  notifications and automation-rule engines.
- No SDL; mbedTLS remains the pinned transport dependency.

## Architecture

### 1. Shared application model

Add `src/client/app.zig` as the UI-facing owner above network/comic modules.
It contains no platform calls and no socket serialization.

Owned state:

- connection profiles and connect/setup draft;
- connection/status lifecycle;
- joined rooms, active room and per-room transcript/history viewport;
- member roster, selection and member actions;
- composer state, formatting runs, command/history state and whisper target;
- comic/text view mode;
- notification watch list and online overlay;
- automation rule sets and editor drafts;
- profile, avatar and backdrop selection;
- DCC offers/transfers and progress;
- preferences, favorites and recent files;
- focus, modal stack, active menu, status announcements and commands.

The model emits typed `Command` values. `main.zig` executes network/file/window
effects and feeds typed results back. UI code never writes IRC strings directly.

### 2. Shared retained UI toolkit

Extend `src/client/` with a small software-rendered retained toolkit:

- `geometry.zig` — rectangles, insets, constraints and scale conversion;
- `theme.zig` — semantic colors, spacing, typography and focus metrics;
- `controls.zig` — button, toggle, text field, list, tabs, menu, scrollbar;
- `focus.zig` — tab order, roving list focus, modal focus stack/restoration;
- `hit_test.zig` — stable control IDs and pointer target resolution;
- `shell.zig` — menu/toolbar, room tabs/list, content split, member pane,
  composer and status bar;
- `dialogs.zig` — modal sheets/dialogs over the same primitives;
- `accessibility.zig` — semantic descriptions/status stream for native adapters
  and test assertions.

The toolkit is immediate in raster output but retained in interaction identity:
controls have stable IDs, focus and activation state. Layout and interaction are
pure/testable without a display server.

### 3. Unified platform contract

Replace the three duplicated minimal `Key`/`Event` unions with one shared
`src/platform/event.zig` contract:

- logical key plus modifiers and press/release/repeat;
- UTF-8 text input independent of key events;
- pointer enter/leave/move/button;
- vertical/horizontal wheel/axis scrolling;
- resize, expose, focus, close;
- logical scale-factor changes;
- clipboard offer/change and asynchronous text result;
- IME preedit/commit/delete-surrounding events where available.

Each backend maps native input into this contract. The shared app handles all
navigation, hit testing, shortcuts and editing once.

### 4. Wayland completion

Implement directly against the Wayland protocol:

- `wl_pointer` motion/buttons/axis and seat capability changes;
- `wl_output` enter/leave and integer buffer scale;
- buffer reallocation and logical-to-buffer coordinate conversion;
- `wl_data_device_manager` clipboard selection, offers and text transfer;
- expanded compositor keymap level/modifier parsing;
- compose/dead-key state;
- `zwp_text_input_v3` preedit/commit when the compositor advertises it;
- graceful capability absence and automatic X11 fallback when Wayland open
  fails before application state is established.

The same pointer, clipboard and scale contract is implemented for X11 and
Win32 so features do not become Wayland-only.

### 5. Persistence

Add bounded, versioned, atomic stores under `src/client/` for:

- preferences and window/UI state;
- connection profiles/favorites;
- notifications;
- automation rules;
- recent files.

Secrets remain outside general preferences. Existing session and STS stores
retain their specialized security boundaries. POSIX files use owner-only mode
when they contain identity or connection metadata.

## Product surfaces

### Shell

- Application menu and shortcut command system.
- Main/member/text toolbars represented as one responsive command bar.
- Room tabs/list, status view and member list.
- Comic strip and plain-text transcript modes.
- Scrollable history with stable tail-follow behavior and jump-to-latest.
- Status bar with connection, room, selection and transfer state.

### Connect and rooms

- New connection/setup screen with server, port, TLS, proxy and SASL choices.
- Saved profiles and favorites.
- Join/leave/create room.
- Room list and room properties.
- MOTD and away state.

### Composer and editing

- UTF-8 multiline input.
- Selection, undo/redo, cut/copy/paste, delete and select-all.
- Input history and completion.
- Bold, italic, underline, fixed-pitch, symbol and color formatting state.
- `/me`, avatar selection and typed slash-command routing.
- Whisper/private-message target and dedicated conversation surface.

### Members and moderation

- List/icon member views and dynamic PREFIX roles.
- Profile/identity/version/lag/local-time requests.
- Notify/watch, ignore, invite and whisper actions.
- DCC file send/receive with consent, path selection and progress.
- Operator actions: kick, ban/unban, host/speaker/spectator and backdrop sync,
  gated by advertised server capability and current privilege.

### Comic settings

- Character/avatar browser using bundled/provenanced assets.
- Background browser and sync controls.
- Personal profile editor.
- Comic/text/font/preferences pages.
- Body-camera pose/expression selection where the source exposes it.

### Notifications and automation

- Notification watch-list editor and online/offline view.
- Automation rule-set list/editor using the ported rule engine.
- Versioned text persistence rather than undocumented Registry binary format.
- Explicit enable/disable, ordering and flood-safety feedback.

### Files

- Open/save `.ccc` conversations.
- Open/save `.ccr` locators and favorites.
- Export comic page to PNG/PPM.
- Print is optional unless a portable print backend is selected.

## Accessibility and interaction contract

- Complete keyboard access and visible focus.
- One tab stop per composite list; arrows move within it.
- Modal focus trap and restoration.
- Minimum 24-device-independent-pixel pointer targets at scale 1.
- Status announcements separated from transcript/history changes.
- Reduced-motion preference; no required information conveyed only by motion
  or color.
- High-contrast semantic theme and no color-only role distinction.
- History insertion does not announce or focus-steal.

Native screen-reader bridge support is a separate backend deliverable because
Wayland has no single accessibility wire protocol equivalent to Win32 UIA.
The shared semantic tree/status stream is implemented first so AT-SPI/UIA
adapters can consume the same model.

## Microsoft parity classification

### Implement

- File/Edit/View/Format/Room/Member/Favorites product workflows.
- Connect/setup, room list, member list, status/text/comic views.
- Settings, character, background, profile, notifications and automation.
- Whisper, moderation and DCC workflows supported by live protocol substrate.
- Saved conversations and locators.

### Portable implementation of the same UI contract

- The original room-tab surface remains; portable window ownership replaces
  MFC MDI mechanics without changing the visible tab behavior.
- Registry persistence becomes bounded atomic versioned files.
- MFC property sheets become shared software-rendered dialogs.
- GDI clipboard/printing/export becomes platform clipboard plus PNG/PPM export;
  print requires an explicit backend decision.
- Comic Sans MS remains Comic Neue in the portable lane.

### Proposed explicit non-goals

- OLE document-server embedding.
- NetMeeting launch/integration.
- Retired Microsoft web/support/free-stuff links.
- Internet Explorer-era HTML help engine.
- Automatic download/execution of untrusted remote avatar code or content.
- Byte-compatible Registry binary persistence.

These non-goals require product approval because “implement all” could be read
as including obsolete integrations.

## Ordered implementation waves

### Wave 1 — Interaction foundation

- Shared event contract, UTF-8 editor, commands, focus, geometry and hit tests.
- Pointer/scroll on all platforms.
- Wayland scale and clipboard substrate.
- Pure tests plus backend mapping tests.

### Wave 2 — Complete shell

- App model, command bar/menu, rooms, scrollable comic/text views, member pane,
  status bar and responsive layout.
- Move connection/message orchestration out of `main.zig` into typed app state.

### Wave 3 — Connect, preferences and persistence

- Graphical connect/setup and saved profiles.
- Preferences, favorites, recent files and window state.
- Onyx client certificate/session options surfaced without exposing secrets.

### Wave 4 — Member and comic workflows

- Profiles, identities, whispering, ignore/notify, avatar/background/profile
  settings and moderation actions.

### Wave 5 — Automation and transfers

- Notification and rule editors/persistence.
- DCC offer consent, progress, cancellation and safe file paths.

### Wave 6 — Files, polish and parity closure

- `.ccc`/`.ccr` open/save, export, optional print decision.
- Compose/IME completion, accessibility adapters, HiDPI polish.
- Microsoft menu/action parity audit and documentation reconciliation.

## Verification

Every wave requires:

```sh
zig fmt --check build.zig src
zig build test
zig build
git diff --check
```

Touched platform waves additionally require relevant Linux/Windows cross-builds,
X11 virtual-display smoke, Wayland compositor smoke when available, Win32 smoke
on Windows, deterministic framebuffer images and backend event-contract tests.

Release claims require a fresh opposite-engine review and inspection of the
actual native application surface.

## Decisions required before production implementation

1. Approve the proposed obsolete-integration non-goals.
2. Decide whether full Wayland compose/IME may add a pinned `libxkbcommon`
   dependency, or must remain an in-tree protocol/keymap implementation.
3. Decide whether portable printing is required in the first parity milestone.
4. Resolved: retain the original visible room tabs while replacing only the
   underlying MFC MDI mechanics.
