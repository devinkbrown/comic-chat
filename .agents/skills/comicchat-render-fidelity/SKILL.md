---
name: comicchat-render-fidelity
description: Port, review, and verify source-faithful Comic Chat layout, avatars, balloons, text, raster composition, and visual output. Use when changing portable rendering/assets/layout code, modern Windows comic drawing, source-derived constants, visual goldens, or fidelity claims that must be traced to immutable Microsoft source and artwork.
---

# Comic Chat render fidelity

## Establish the visual oracle

1. Read AGENTS.md, portable/README.md, and references/source-oracle-map.md.
2. Name the exact Microsoft routine, constant, resource, or authored asset that defines the requested behavior.
3. Trace its callers and data model in v2.5-beta-1/ before inspecting the modern implementation.
4. Capture a deterministic baseline image, hash, geometry record, or focused test failure.
5. Change only the modern lane: portable/ for the native port or the matching -modern/ Windows tree.

Never claim parity from screenshots alone, a placeholder, or visual plausibility. Treat the original source and authored assets as the behavioral authority.

## Port behavior, not accidents

- Preserve the 1,440-unit logical coordinate model and defer device scaling until the presentation boundary.
- Keep layout, text measurement, avatar selection, balloon routing, and raster composition independent of SDL presentation.
- Preserve source ordering, random-seed flow, rounding, retry, clipping, mask, and raster-operation semantics when they affect output.
- Distinguish authored asset geometry from runtime scaling. Do not redraw Microsoft reference art as an implementation shortcut.
- Keep deterministic mode stable for tests and goldens. Do not make a passing golden depend on worker timing, locale, system font fallback, or display server.
- Use the shared TextEngine and bundled/source-selected font path. Test shaping, measurement, ellipsis, and rasterization together when glyph metrics change.
- Keep legacy_exact and modern_remaster semantics explicit. Do not mix remastered icon or UI policy into source-exact comic rendering.
- Record the upstream routine beside any changed source-derived constant or golden hash.

## Respect the current completeness boundary

Treat portable/README.md as the current claim ledger. The title-panel foundation does not imply that avatars, body poses, balloons, expert placement, or the complete room/page shell are finished.

- Add a missing source-derived subsystem behind a tested narrow API.
- Keep placeholders visibly labeled and out of parity claims.
- Avoid a second heuristic path. Extend the source-derived pipeline rather than adding a screenshot-tuned special case.
- Preserve immutable snapshots and original resource bytes.

## Remaster complete characters, not fragments

Treat a character remaster as a source-topology preservation task. The
character's body, face, hair, costume, limbs, pose, expression, prop, shadow,
and authored occlusion order form one design; never replace that composition
with a generic face, face-with-legs, silhouette, or unrelated mascot.

1. Use `portable/tools/avatar_oracle.cpp` and the historical AVB/source path to
   render `legacy_exact` at its native dimensions.
2. Record stable landmarks and layer relationships: head and eye centers,
   torso/hip/hand/foot anchors, body-to-face scale, gesture attachment,
   silhouette extrema, palette, mask, and transparent bounds.
3. Render `modern_remaster` from the same records, pose/expression indices,
   anchors, crop, and destination rectangle. Limit automatic remastering to
   source-derived reconstruction, sampling, edge cleanup, and high-resolution
   ink treatment unless the owner explicitly approves new authored artwork.
4. Compare the complete character at native size and representative 150%,
   200%, and 400% outputs. A smooth face does not pass when the body, pose,
   proportions, costume, or silhouette changed or disappeared.
5. Keep `legacy_exact` as the immutable behavioral oracle and add a focused
   geometry/topology assertion plus before/after visual sheet for every
   remastered character or pose family.

Do not infer missing anatomy from a cropped screenshot. Resolve it from the
original body/pose records and artwork; if the required source is absent, mark
the remaster blocked rather than inventing a body.

## Produce reviewable evidence

Use headless deterministic output for causal review:

~~~sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  ./<build-dir>/comic-chat --frames 2 --png <output.png>
~~~

- Compare geometry and pixels only after proving identical inputs, font, asset, scale, random seed, and deterministic scheduling.
- Keep before/after images, dimensions, hashes, and the source citation together.
- Inspect compact details and edge cases at native scale; do not judge only a zoomed composite.
- Add a focused test in layout_test.cpp, render_test.cpp, source_raster_test.cpp, avatar_assets_test.cpp, text_test.cpp, or a new registered target as appropriate.
- Run the complete headless suite after the focused test. Run the real Wayland smoke and native Windows gate when presentation or platform resources change.
- Run python3 scripts/build-modern-icons.py verify when generated icon consumers or modern UI resources are touched.

Report what became source-faithful, what remains a placeholder, the exact oracle path/routine, deterministic reproduction command, visual artifact, test results, and unresolved cross-platform differences.
