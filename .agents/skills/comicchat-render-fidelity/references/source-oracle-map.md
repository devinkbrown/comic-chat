# Rendering source oracle map

## Immutable historical oracle

Read these paths under v2.5-beta-1/ and never edit them:

| Behavior | Primary source |
| --- | --- |
| Logical units and minimum panel dimensions | defines.h |
| Two-column page, gutters, title/starring layout, roster ordering | panel.cpp and panel.h |
| Page logical-to-device drawing | pageview.cpp |
| Balloon shape, text fit, tails, and routing | balloon.cpp and balloon.h |
| Participant/history state and source-driven reactions | histent.cpp and histent.h |
| Avatar/body composition | avatar.cpp, bodycam.cpp, and related headers |
| AVB/BGB records and bitmap/mask semantics | avbfile.cpp and avbfile.h |
| Font selection and measurement | fonts.cpp |
| Windows raster and DIB behavior | dib.cpp and artifacts/core/dib.cpp |
| UI/resource identity | res/, chat.rc, and resource.h |

Use other historical versions only to answer an explicit version-difference question. Do not blend behavior across snapshots accidentally.

## Modern implementation map

| Modern behavior | Portable path | Primary tests |
| --- | --- | --- |
| Panel geometry and roster order | portable/include/comicchat/layout.hpp, portable/src/layout.cpp | portable/tests/layout_test.cpp |
| Cairo comic canvas and title rendering | portable/include/comicchat/render.hpp, portable/src/render.cpp | portable/tests/render_test.cpp |
| Source raster extraction | portable/include/comicchat/source_raster.hpp, portable/src/source_raster.cpp | portable/tests/source_raster_test.cpp |
| AVB avatar parsing and composition | portable/include/comicchat/avatar_assets.hpp, portable/src/avatar_assets.cpp | portable/tests/avatar_assets_test.cpp |
| Legacy-exact and high-resolution avatar oracle | portable/tools/avatar_oracle.cpp | modern-remaster topology cases in portable/tests/avatar_assets_test.cpp |
| Unicode shaping and metrics | portable/include/comicchat/text.hpp, portable/src/text.cpp | portable/tests/text_test.cpp |
| Bounded immutable render batches | portable/include/comicchat/memory.hpp, portable/src/memory.cpp | portable/tests/memory_test.cpp |
| Deterministic scheduling | portable/include/comicchat/scheduler.hpp, portable/src/scheduler.cpp | portable/tests/runtime_test.cpp |
| SDL presentation and PNG capture | portable/src/app.cpp | headless and Wayland smoke |

Modern Windows source lives in v2.5-beta-1-modern/. Keep shared rendering rules aligned while preserving its native GDI/MFC ownership and message flow.

## Primary external sources

- Microsoft Comic Chat source repository: https://github.com/microsoft/comic-chat
- Cairo image surfaces: https://www.cairographics.org/manual/cairo-Image-Surfaces.html
- FreeType glyph loading: https://freetype.org/freetype2/docs/reference/ft2-glyph_retrieval.html
- HarfBuzz shaping: https://harfbuzz.github.io/shaping-and-shape-plans.html
- ICU Unicode services: https://unicode-org.github.io/icu/userguide/
- SDL3 surfaces: https://wiki.libsdl.org/SDL3/CategorySurface

External library documentation defines API behavior, not Comic Chat appearance. Resolve visual behavior from Microsoft's source and assets first.
