# HD Comic Chat source asset library

This directory contains non-destructive, high-resolution source artwork for
the next Comic Chat visual refresh. It is intentionally separate from the
runtime AVB/BGB assets: those legacy containers include pose, icon, mask, and
compression records and cannot be safely replaced by a single flattened PNG.

## Avatar sources

- `avatar-reimagined-hd-v2/roster-reimagined-hd-v2.png` is the preferred full
  HD reimagining pass for the entire legacy cast. Its adjacent,
  name-mapped `*-reimagined-hd-v2.png` crops provide a source file for every
  existing avatar identity while retaining their species and broad silhouette.
- `avatar-color-hd-v1/roster-color-hd-v1.png` is the complete 22-character
  colored roster.
- `avatar-color-hd-v1/<legacy-name>-color-hd-v1.png` maps a cropped source
  tile to every existing avatar name: Anna, Armando, Bolo, Cro, Dan, Denise,
  Hugh, Jordan, Kevin, Kwensa, Lance, Lynnea, Margaret, Maynard, Mike, Rebecca,
  Sage, Scotty, Susan, Tiki, Tongtyed, and Xeno.
- `avatar-color-hd-v1/tiki-color-hd-v2.png` is the preferred Tiki palette:
  carved warm wood, terracotta markings, teal eye rings, ivory teeth, and a
  deep teal body. It supersedes the high-neon v1 experiment.
- `avatar-color-hd-v1/tiki-original-style-v3.png` is the preferred new Tiki
  character direction: an original balanced leaf-collar design with carved
  wood arms and head, deep-teal trousers, coral sandal bands, and a playful
  asymmetrical mask expression. It is inspired only broadly by the old mask
  avatar rather than being a redraw.
- `avatar-refresh-v1/` retains the original four monochrome redraw sources.

## Backdrop sources

- `backgrounds-mono-hd-v1/` contains ten black-ink, paper-white scenes:
  apartment, rooftop, cafe, park, space corridor, boardwalk, school hall,
  rainy street, library, and campsite.
- `backgrounds-color-hd-v1/` contains the corresponding ten color scenes,
  plus ten surreal color scenes: spaceship bridge, asteroid diner, sky-island
  market, underwater dome, friendly castle, pinball interior, cosmic
  laundromat, cloud train station, mushroom village, and arcade planetarium.
- Each sheet is retained for review, with crop tiles alongside it.

## Native runtime package

Every legacy identity now has an approved generated runtime AVB under
`src/assets/generated/`. Each is a native simple-avatar package with an icon
and six authored body records (neutral, happy, surprised, angry, sad, and
action), built from its named pose-source set. The character dialog exposes
all twenty-two as **Name HD** choices. `AVB_SHA256SUMS.txt` records every
runtime package digest; the package format, source path convention, license
boundary, and rebuild command live in
[`src/assets/generated/README.md`](../../src/assets/generated/README.md).

## Runtime integration gate

Before a source image becomes a bundled runtime asset, create its matching
legacy pose/icon/mask data and package it as AVB or BGB. Validate source-size
rendering, alpha/mask ordering, and every emotion or body layer. The existing
protocol and old-client compatibility path remains independent of this visual
source library.

## Pose-source sets

Each `avatar-pose-sheets-v1/<name>/` directory contains a generated pose sheet
and six crop tiles in stable order: neutral, happy, surprised, angry, sad,
and action. These are the exact packaging inputs for that name's native AVB.
