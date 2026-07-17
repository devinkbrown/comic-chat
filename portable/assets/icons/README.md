# Comic Chat: Reinked icon masters

This directory is the editable source of truth for the replacement icon set.
The Microsoft bitmaps in `portable/assets/source-raster` are reference material, never build
inputs for the new artwork.

## Layout

Standalone Windows icons use one 256-unit square master plus authored 16px and
32px optical drawings. The cameo-heavy `chat` and `avatar` families also have
dedicated 20px and 24px redraws; those intermediate frames for the other nine
families intentionally render from the 32px optical drawing and are not claimed
as separately hand-authored:

```text
masters/chat.svg
masters/avatar.svg
...
optical/16/chat.svg
optical/20/chat.svg
optical/24/chat.svg
optical/32/chat.svg
...
```

Toolbar and status glyphs are namespaced by their native strip. This prevents
same-named subjects such as `away` and `whisper` from being accidentally reused
when their authored contexts differ:

```text
masters/strips/toolbar/connect.svg
masters/strips/member/away.svg
optical/16/strips/toolbar/connect.svg
optical/24/strips/member/away.svg
```

The runtime strip contract covers `toolbar`, `tabbar`, `balloons`, `member`,
`texttool`, `usertool`, `connect`, `oldnew`, `inactive`, `active`, and
`stopped`. Every source is an individual square cell; the generator assembles
equal-cell 16/20/24/32/40/48 PNG and BMPv4 ladders in manifest order. Optional
compact strip redraws use the same optical directory pattern.

Bodycam expressions use a 20:26 master canvas:

```text
masters/expressions/happy.svg
optical/20x26/expressions/happy.svg
```

The 20x26 override is required for all eight expressions. Larger native sizes
derive from the master unless an exact `optical/<width>x<height>` redraw exists.

All SVGs must have a square `viewBox`, remain vector-only, and contain no
scripts, event handlers, embedded rasters, external references, or remote
fonts. Compact overrides are real redraws, not generated reductions.

## Deterministic build

From the repository root:

```sh
python3 scripts/build-modern-icons.py lint --complete
python3 scripts/build-modern-icons.py generate
python3 scripts/build-modern-icons.py verify --rebuild
```

`generate` is fail-closed while `manifest.json` marks the current art revision
as blocked. The review marker may move to `approved` only after the face-bearing
application/document icons and cameo toolbar glyphs pass visual review; missing
strip or expression masters can never be replaced by placeholders.

`generate` renders eight uncompressed 32-bit DIB frames per Windows ICO
(including 20 px and 24 px native UI frames), a direct PNG ladder from the same
masters for SDL/Wayland, and six alpha-preserving BMPv4 strips per strip family.
PNG-compressed ICO frames are deliberately forbidden because the existing
Windows-DIB decoder consumes ICO frames directly; the portable runtime never
needs to decode the ICO and instead loads the PNG ladder.

Generation is staged under `portable/assets/icons/generated`. It is the single
canonical modern runtime asset tree: Windows resources consume its ICOs and
size-specific BMPv4 strips, while SDL consumes its PNG ladder and publishes
32/64/128/256 surfaces to Wayland. `verify` checks the complete catalog and
proves every Windows replacement differs from the corresponding Microsoft
reference. The original source tree is never modified.
