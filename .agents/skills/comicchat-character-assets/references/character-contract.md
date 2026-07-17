# Character remaster contract

## Immutable Microsoft evidence

The active shipped corpus is under `v2.5-beta-1/ComicArt/`. Treat each `.avb`
as a structured character family, not a single bitmap. The 25 active character
files are enumerated by `portable/tests/avatar_assets_test.cpp` and must remain
loadable as a corpus.

Read these historical sources before changing behavior:

| Contract | Microsoft source |
| --- | --- |
| AVB tags, palettes, images, masks, aura, and component records | `v2.5-beta-1/avbfile.cpp`, `avbfile.h` |
| Character ownership and pose selection | `v2.5-beta-1/body.cpp`, `body.h` |
| Simple whole-body composition | `v2.5-beta-1/bodyunry.cpp`, `bodyunry.h` |
| Complex face/torso composition and anchors | `v2.5-beta-1/avatar.cpp`, `avatar.h` |
| Preview/bodycam state and expression input | `v2.5-beta-1/bodycam.cpp`, `bodycam.h` |
| Panel placement, flip, scale, and occlusion | `v2.5-beta-1/panel.cpp`, `pageview.cpp` |
| DIB mask and raster-operation semantics | `v2.5-beta-1/dib.cpp`, `artifacts/core/dib.cpp` |

The archived executable/property-page render is the final disambiguation
oracle when comments and reconstructed code leave a raster or selection detail
unclear. Record how the capture was produced.

## Modern implementation evidence

| Contract | Modern path |
| --- | --- |
| Bounded AVB model and render request | `portable/include/comicchat/avatar_assets.hpp` |
| AVB decoder, expression selection, masks, composition, and remaster filter | `portable/src/avatar_assets.cpp` |
| Corpus, topology, flip, Tiki anatomy, and high-DPI landmark checks | `portable/tests/avatar_assets_test.cpp` |
| Reproducible source/remaster PNG output | `portable/tools/avatar_oracle.cpp` |
| Comic panel integration | `portable/src/render.cpp`, `portable/src/app.cpp` |

`AvatarRenderMode::legacy_exact` is the compatibility oracle.
`AvatarRenderMode::modern_remaster` may improve contour reconstruction but must
retain the selected components and geometry. Do not silently make the default
oracle mode depend on display scale.

## Required per-character inventory

Before a remaster, record:

- file hash, kind, name, style, flags, and icon pose;
- every decoded pose's drawing, mask, aura, dimensions, and palette role;
- every body, face, and torso record with pose ID, emotion, intensity, centers,
  center deltas, and face anchors;
- every selection reachable for representative angle/intensity sweeps;
- normal and flipped output, nimbus states, native preview crop, and panel crop;
- source-visible anatomy landmarks and intentionally asymmetric details.

The inventory is part of the review artifact. It prevents a remaster from
being judged only by a single favorable pose.

## Acceptance failures

Reject a character revision if any of these occur:

- the full body in the source state becomes a head, bust, or face with legs;
- limbs, torso, face, accessories, or shadows detach across states;
- face or torso anchors jump between adjacent expressions;
- a state, intensity, mask, aura, flip, or palette family is missing;
- the character is recognizable only because a name or surrounding UI says so;
- high-DPI output is merely nearest-neighbor enlargement, generic AI detail,
  excessive blur, or a different art direction;
- generated assets lack a deterministic master, manifest, provenance, or
  reproducible clean-build check;
- native-size readability regresses even though a large preview looks good.

## External API references

- Cairo image surfaces: https://www.cairographics.org/manual/cairo-Image-Surfaces.html
- Cairo transformations: https://www.cairographics.org/manual/cairo-Transformations.html
- SDL3 surface APIs: https://wiki.libsdl.org/SDL3/CategorySurface

These sources define raster APIs only. Microsoft source, AVB metadata, and the
archived client define the character design and composition.
