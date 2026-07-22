# Avatar refresh source set — v1

These are high-resolution, monochrome source redraws created as faithful
updates of four existing Comic Chat avatars:

- `anna-refresh-v1.png`
- `armando-refresh-v1.png`
- `xeno-refresh-v1.png`
- `tongtyed-refresh-v1.png`

They preserve each character's recognizable silhouette and use the existing
archive's black-ink-on-white-paper visual language. They are intentionally
kept as versioned PNG source artwork. The runtime still consumes legacy `.avb`
assets, which carry separate icon, face, torso, mask, and expression-pose
records; these PNGs must not replace those containers directly.

Before runtime integration, derive a compatible icon and the complete posed
layer set from an approved redraw, then package and validate the matching AVB
records together.
