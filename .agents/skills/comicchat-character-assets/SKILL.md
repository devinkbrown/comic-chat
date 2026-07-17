---
name: comicchat-character-assets
description: Remaster, upscale, review, and verify complete Microsoft Comic Chat characters and pose families while preserving anatomy, identity, expression, composition, and deterministic AVB-driven behavior. Use when changing character artwork, pose topology, faces, torsos, bodies, masks, aura layers, high-DPI avatar rendering, character manifests, or character visual goldens.
---

# Comic Chat character assets

## Establish the complete character oracle

1. Read `references/character-contract.md`, `docs/MODERNIZATION.md`, and the
   character sections of the render-fidelity source-oracle map.
2. Name exactly one Microsoft `.avb` character and inventory every pose,
   body, face, torso, center, delta, face anchor, mask, aura, palette, flip,
   and expression-selection record before editing.
3. Trace those records through `CBody`, `CBodyUnary`, `CAvatar`, and
   `CBodyCam` in `v2.5-beta-1/`, then through the portable AVB decoder,
   selector, compositor, and avatar oracle. Never edit the historical tree.
4. Render a complete baseline contact sheet from the source asset. A cropped
   screenshot, icon, or neutral face is not a character oracle.
5. Define source-derived topology and landmark checks before authoring a
   remaster. If the complete body or pose family cannot be recovered, stop and
   report the missing oracle rather than inventing it.

Use `comicchat-render-fidelity` for compositor behavior and panel placement.
Use `comicchat-icon-assets` only for the separate small avatar icon or bodycam
expression catalog; it is not the character-remaster procedure.

## Preserve identity before adding polish

- Preserve the complete body, head-to-body ratio, limb attachment, stance,
  pose direction, face placement, expression, silhouette, palette
  relationships, outline rhythm, negative space, and center of mass.
- Preserve simple versus complex avatar composition. Do not flatten a complex
  face/torso family into unrelated whole-body drawings or splice independently
  generated parts whose anchors do not match.
- Preserve the source pose IDs, emotion indices, intensity ordering, center
  deltas, face anchors, flip behavior, masks, aura/nimbus behavior, and fallback
  selection semantics. A beautiful neutral drawing is not a valid replacement
  for a broken state family.
- Never create a floating face, a face with legs, a generic mascot, a crude
  silhouette, or a new anatomy interpretation. Do not infer hidden anatomy
  from one screenshot when the AVB components or original renderer expose it.
- Add modern polish only after topology matches: clean sub-pixel contours,
  controlled line-weight variation, coherent material shading, restrained
  highlights, restored color separation, and high-resolution edges.
- Keep authored masters, source-derived metadata, and generated rasters
  distinct. Do not overwrite the Microsoft AVB, hand-edit generated outputs,
  or use generated output as the next generation's source.

## Keep the family coherent

- Review every body/face/torso combination that the selector can emit, not
  just the neutral hero pose.
- Keep facial landmarks and torso anchors stable across emotion and intensity
  changes so the animation does not jump, shrink, or detach.
- Preserve handedness and asymmetric details when flipping. Mirror only the
  layers and coordinates mirrored by the Microsoft compositor.
- Keep the character recognizable at the historical 149x133 preview and at
  150%, 200%, and 400% scale. High resolution must reveal intentional detail,
  not enlarged source pixels or invented microtexture.
- Evaluate the character on light and dark backdrops and inside an actual
  Comic Chat panel with balloons, occlusion, and roster scaling.
- Remaster one complete character family at a time. Do not partially ship a
  new face style with legacy bodies unless an explicit compatibility design
  and complete mixed-state test prove it.

## Require deterministic evidence

Generate both legacy-exact and remastered contact sheets with
`comicchat-avatar-oracle`. Keep the same source AVB, selection sequence,
canvas, background, flip state, and random seed for every comparison.

For each emitted state, compare:

- non-background bounds and occupancy centroid;
- head, face, shoulder/torso, hand, and foot landmarks where visible;
- mask and aura registration;
- component-anchor continuity across adjacent expressions;
- source palette families and outline contrast;
- silhouette topology at native size;
- full-body visibility and intended panel crop.

Use numeric landmark tolerances to catch anatomy drift, but never treat a
similarity score as aesthetic approval. Inspect native-size output at 1:1 and
the high-DPI output without browser or image-viewer smoothing.

## Verify all consumers

Run the focused asset suite and deterministic oracle:

~~~sh
meson test -C <build-dir> comicchat-avatar-assets --print-errorlogs
<build-dir>/comicchat-avatar-oracle --help
~~~

Then run the headless render suite and both native frontend/package gates when
asset lookup, metadata, compositor behavior, or installed data changes. For a
new remaster source pipeline, add a manifest with hashes and a clean rebuild
verification step before any generated result becomes a build input.

Capture contact sheets for every source state plus neutral/east/west and
minimum/maximum intensity selections at 100%, 150%, 200%, and 400%. Record the
Microsoft AVB path, original source symbols, selection sequence, source and
remaster hashes, exact commands, native-size inspection, landmark results,
remaining mixed-state risks, and explicit owner approval of the art revision.
