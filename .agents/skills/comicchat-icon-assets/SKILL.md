---
name: comicchat-icon-assets
description: Author, regenerate, review, and verify Comic Chat's deterministic modern native icon catalog. Use when changing SVG masters or optical redraws, manifest coverage, Windows ICO/BMP resources, SDL/Wayland PNG icons, bodycam expressions, icon generation, resource IDs, packaging metadata, or icon-related visual and CI failures.
---

# Comic Chat icon assets

## Use the declared asset pipeline

1. Read portable/assets/icons/README.md, portable/assets/icons/manifest.json, and references/catalog-contract.md.
2. Identify the semantic resource, every required size, each native consumer, and the original Microsoft reference.
3. Edit only a declared master or optical SVG, or the generator/manifest when the task explicitly changes the catalog contract.
4. Run lint before generation, regenerate the complete catalog, then verify from a clean rebuild.
5. Inspect the generated output at native sizes before accepting mechanical success.

Never edit files under portable/assets/icons/generated/ by hand. Never use v2.5-beta-1/res as a build input for new artwork.

## Preserve authored sources

- Keep each SVG vector-only with a valid viewBox and no scripts, event handlers, embedded rasters, external references, remote fonts, or text elements.
- Use masters/ for the scalable design and optical/ for intentional compact redraws. Do not pass a generated reduction off as an authored optical size.
- Preserve all standalone icon families, strip families and cell order, expression families, masks, resource IDs, application identity, and size ladders declared in manifest.json.
- Keep the 16px and 32px standalone optical drawings complete. Keep explicit 20px and 24px standalone redraws only where the manifest requires them.
- Keep all eight 20x26 bodycam expression optical drawings. Preserve their canonical emotion order and generated resource offsets.
- Keep strip subjects namespaced by strip family; same-named subjects can have different context and geometry.
- Do not change art_review status, revision, or reason without explicit project-owner approval of the reviewed art revision.

## Generate deterministically

Run from the repository root:

~~~sh
python3 scripts/build-modern-icons.py lint --complete
python3 scripts/build-modern-icons.py generate
python3 scripts/build-modern-icons.py verify --rebuild
~~~

- Preserve uncompressed 32-bit DIB frames in Windows ICO output.
- Preserve the direct PNG ladder for SDL/Wayland and alpha-preserving BMPv4 ladders for Windows strip resources.
- Preserve canonical manifest ordering and catalog.lock.json provenance.
- Fail when a required source, optical drawing, renderer, resource declaration, quality gate, or generated file is missing.
- Keep the modern catalog visibly distinct from the Microsoft reference resources while retaining semantic recognizability.

## Review visual quality

- Inspect 16, 20, 24, and 32px outputs at 1:1 scale. Check silhouette, face identity, stroke survival, contrast, alpha edges, mask color, and active/inactive states.
- Inspect 48, 64, 128, and 256px standalone outputs for path defects and unintended detail.
- Inspect complete strips in manifest cell order and all expression sizes.
- Test light, dark, high-contrast, and transparent backgrounds where the platform uses them.
- Treat the generator's pixel statistics as mechanical guards, not aesthetic approval.

## Verify every consumer

Run:

~~~sh
python3 v2.5-beta-1-modern/tests/modern_icon_pipeline_test.py
python3 v2.5-beta-1-modern/tests/windows_icon_integration_test.py
python3 v2.5-beta-1-modern/tests/dialog_chrome_test.py
meson test -C <build-dir> comicchat-modern-icon-catalog --print-errorlogs
~~~

Compile and smoke the portable frontend and the native Windows resource build after resource IDs, generated includes, app identity, or runtime lookup changes. Check portable/assets/icons/meson.build, portable/src/app.cpp, v2.5-beta-1-modern/chat.rc, modernicons.cpp, modernui.cpp, and chat.mak as applicable.

Report the source SVGs, generated catalog diff, lint/generate/verify results, visual review sizes, consumer tests, and explicit owner approval when the art revision changed.
