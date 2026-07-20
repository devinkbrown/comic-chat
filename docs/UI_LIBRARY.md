# Desktop UI library

Comic Chat's desktop chrome is rendered by the portable Zig UI library in
`src/client/ui.zig`. Native X11, Wayland, Win32, FreeBSD, and OpenBSD backends
all present the same framebuffer and therefore share the same interaction and
visual contract.

## Design contract

- Application chrome uses the embedded neutral sans-serif atlas in
  `src/render/font_ui.zig`. Comic Neue is reserved for authored comic content.
- The shell uses a restrained ink-and-drafting-blue system: paper-white work
  surfaces, graphite chrome, and one cobalt interaction color.
- The established 80/20 conversation/inspector and 30/70 member/character
  proportions remain recognizable, while control heights and spacing are
  sized for a modern desktop.
- The radial mood dial remains the signature interaction. It supports direct
  click, captured drag, arrow-key adjustment, Home-to-neutral, a visible
  intensity puck, and a right-click Freeze/Character/Neutral menu.
- Mood expressions use a newly drawn, high-contrast face set with consistent
  geometry rather than the original tiny pixel marks.
- An empty conversation renders as a responsive blank comic page with real
  panel gutters. At minimum window sizes it collapses to a compact instruction
  instead of overflowing the available buffer.
- Comic pages default to four panels across. The room strip exposes an
  accessible minus/plus stepper for live one-to-six-column density changes;
  rendered pages remain top-aligned as conversation history grows. Desktop
  pages reserve the selected column count even when a row contains only one
  message or a break control, preventing sparse panels from being scaled to
  the full buffer.
- Draw geometry, pointer targets, focus, and accessibility bounds must derive
  from the same layout values.
- The desktop surface enforces a 640x480 minimum. Below 760 pixels wide the
  inspector collapses so the composer and comic buffer remain usable.
- Roster and character previews use alpha-aware smooth scaling and larger
  portrait targets. Authored art inside comic panels stays source-faithful.
- The inspector is one continuous rail rather than nested gray cards. Members
  support icon/list modes, keyboard roving, wheel scrolling, retained viewport
  position, selection, hover, and context menus. Visible cards always map back
  to their actual live-roster entry after scrolling.
- History and long rosters expose deterministic scroll position indicators.
- Dialog fields are typed: text, password, choice, list, preview, and read-only
  controls render and interact according to their actual purpose. The shared
  input surface includes hover and focus states, a focus halo and baseline,
  secure-entry adornment, an explicit caret-to-placeholder gap, selection,
  mouse caret placement, horizontal caret tracking for long values, and
  visible validation. `DialogLayout` reserves a dedicated notice/progress row
  so a five-field transfer or administration dialog cannot paint validation
  over its final input or action buttons. Character and backdrop choices cover
  every bundled asset and show live previews.
- The condensed toolbar is intentionally modern; commands that do not belong
  in the primary strip remain reachable through menus and context actions.

## Reusable primitives

`Theme` is the color token source. `ControlState` and
`resolveControlColors()` define normal, hovered, selected, focused, pressed,
and disabled behavior. New screens should compose the existing primitives:

- `drawSurface`, `drawRoundedBorder`, and `fillRoundedRect`
- `drawAaDisc`, `drawAaLine`, `drawAaRing`, and `drawAaCircleOutline` for
  supersampled compact icon artwork
- `drawButton`, `drawCommandTile`, `drawActionTile`, and `drawFocusRing`
- `drawPill`, `drawTooltip`, `drawNotice`, and `drawHistoryBanner`
- `drawInputControl`, `InputKind`, `InputState`, `drawComposerField`, and
  `DialogLayout`
- `drawMenuItem`, `drawTab`, `drawMemberCard`, and `drawMessageRow`
- `drawPaneHeader`, `drawExpressionPanel`, and `drawStatusBar`
- `drawInspectorRail` and `drawVerticalScrollbar`

Do not add ad-hoc colors or use Comic Neue for application controls. Add a
token or reusable primitive when a new state is genuinely required.

## Deterministic visual checks

The renderer can produce exact previews without a display server:

```sh
zig build run -- render-ui > ui-preview.png
zig build run -- render-ui conversation > ui-conversation-preview.png
zig build run -- render-ui sparse > ui-sparse-preview.png
zig build run -- render-ui break-only > ui-break-preview.png
zig build run -- render-ui menu > ui-menu-preview.png
zig build run -- render-ui settings > ui-dialog-preview.png
zig build run -- render-ui inputs > ui-input-preview.png
zig build run -- render-ui composer > ui-composer-preview.png
zig build run -- render-ui character > ui-character-preview.png
zig build run -- render-ui context > ui-context-preview.png
zig build run -- render-ui hover > ui-hover-preview.png
zig build run -- render-ui say-hover > ui-say-hover-preview.png
zig build run -- render-ui member > ui-member-preview.png
zig build run -- render-ui compact > ui-compact-preview.png
zig build run -- render-ui compact-menu > ui-compact-menu-preview.png
zig build run -- render-ui compact-settings > ui-compact-settings-preview.png
zig build run -- render-ui dialog-file_transfer > ui-transfer-preview.png
zig build run -- render-ui dialog-room_access > ui-access-preview.png
```

These previews exercise the empty shell, real comic content, menu surface, and
modal surface, populated account/password controls, composer overflow, member
selection, and narrow responsive geometry. The generic `dialog-<enum_name>`
form renders any registered dialog with the exact shared geometry; specialized
sample data is included for IRCX, DCC, automation, notification, profile, and
call-link workflows. Run `zig build test --summary all`
with them; pixel checks cover the shell palette and dial while semantic tests
cover control geometry.

The UI acceptance gate also exercises all 48 registered dialogs at 640x480,
800x600, and 960x720; every menu row at the 640px minimum; Debug and
ReleaseSafe tests; native X11 menu/dialog/composer input under Xvfb; and the
same live Win32 interaction path under headless Wine. The Wine path also
exercises status-bar connection setup, invalid-port recovery, verified-TLS
reconnect, live member scrolling/selection/context actions, Settings, Room
List, User List, Comic View density, sparse-page geometry, and composer input.
Linux and Windows
`render-ui` output must remain byte-identical for the main, compact menu,
settings, password input, long composer, and conversation surfaces.

## Font regeneration

The UI atlas is generated from OFL-licensed Liberation Sans Regular:

```sh
python3 tools/generate_ui_font.py /path/to/LiberationSans-Regular.ttf
```

The generator verifies the pinned source hash. Licensing is recorded in
`src/render/LIBERATION_SANS_LICENSE.txt`.
