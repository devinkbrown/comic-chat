# Desktop UI library

Comic Chat's desktop chrome is rendered by the portable Zig UI library in
`src/client/ui.zig`. Native X11, Wayland, Win32, FreeBSD, and OpenBSD backends
all present the same framebuffer and therefore share the same interaction and
visual contract.

## Design contract

- Application chrome uses the embedded neutral sans-serif atlas in
  `src/render/font_ui.zig`. Comic Neue is reserved for authored comic content.
- The shell uses layered Fluent-style neutrals with one Microsoft-blue accent.
- The established 80/20 conversation/inspector and 30/70 member/character
  proportions remain recognizable, while control heights and spacing are
  sized for a modern desktop.
- The radial mood dial remains the signature interaction. It supports direct
  click and captured drag selection, including a neutral center.
- An empty conversation renders as a responsive blank comic page with real
  panel gutters. At minimum window sizes it collapses to a compact instruction
  instead of overflowing the available buffer.
- Draw geometry, pointer targets, focus, and accessibility bounds must derive
  from the same layout values.

## Reusable primitives

`Theme` is the color token source. `ControlState` and
`resolveControlColors()` define normal, hovered, selected, focused, pressed,
and disabled behavior. New screens should compose the existing primitives:

- `drawSurface`, `drawRoundedBorder`, and `fillRoundedRect`
- `drawButton`, `drawCommandTile`, `drawActionTile`, and `drawFocusRing`
- `drawPill`, `drawTooltip`, `drawNotice`, and `drawHistoryBanner`
- `drawField`, `drawComposerField`, and `DialogLayout`
- `drawMenuItem`, `drawTab`, `drawMemberCard`, and `drawMessageRow`
- `drawPaneHeader`, `drawExpressionPanel`, and `drawStatusBar`

Do not add ad-hoc colors or use Comic Neue for application controls. Add a
token or reusable primitive when a new state is genuinely required.

## Deterministic visual checks

The renderer can produce exact previews without a display server:

```sh
zig build run -- render-ui > ui-preview.png
zig build run -- render-ui conversation > ui-conversation-preview.png
zig build run -- render-ui menu > ui-menu-preview.png
zig build run -- render-ui settings > ui-dialog-preview.png
zig build run -- render-ui hover > ui-hover-preview.png
```

These previews exercise the empty shell, real comic content, menu surface, and
modal surface. Run `zig build test --summary all` with them; pixel checks cover
the shell palette and dial while semantic tests cover control geometry.

## Font regeneration

The UI atlas is generated from OFL-licensed Liberation Sans Regular:

```sh
python3 tools/generate_ui_font.py /path/to/LiberationSans-Regular.ttf
```

The generator verifies the pinned source hash. Licensing is recorded in
`src/render/LIBERATION_SANS_LICENSE.txt`.
