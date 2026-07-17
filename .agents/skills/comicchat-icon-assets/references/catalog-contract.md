# Modern icon catalog contract

## Source and output ownership

| Asset | Editable source | Generated consumers |
| --- | --- | --- |
| Standalone application/document icons | portable/assets/icons/masters/*.svg and optical/<size>/*.svg | generated/windows/*.ico and generated/png/<name>/<size>.png |
| Toolbar/status/member strips | masters/strips/<family>/<cell>.svg and optical/<size>/strips/... | generated/windows/strips/<family>-<size>.bmp |
| Bodycam expressions | masters/expressions/*.svg and optical/20x26/expressions/*.svg | generated/windows/expressions/<name>-<size>.bmp |
| Catalog identity and order | portable/assets/icons/manifest.json | catalog.lock.json, generated make/resource includes, Meson install data |

The reference_root v2.5-beta-1/res contains immutable Microsoft artwork used for semantic comparison and distinctness checks. It is not the editable modern source.

## Required ladders

- Standalone ICO and PNG: 16, 20, 24, 32, 48, 64, 128, and 256.
- Windows strips: 16, 20, 24, 32, 40, and 48.
- Expressions: 20x26, 25x33, 30x39, 40x52, 50x65, 60x78, and 80x104.
- Standalone optical redraws: 16 and 32 for every family; 20 and 24 additionally for the manifest's cameo-heavy families.
- Expression optical redraws: 20x26 for all eight families.

Read manifest.json rather than copying these lists into generator code. The manifest and generator validate each other.

## Integration map

- Generator and auditor: scripts/build-modern-icons.py
- Portable install declarations: portable/assets/icons/meson.build
- SDL/Wayland runtime: portable/src/app.cpp and Linux desktop metadata
- Windows generated dependency/resource fragments: generated/windows/modern-icon-assets.makinc and modern-icon-assets.rcinc
- Windows consumers: v2.5-beta-1-modern/chat.mak, chat.rc, modernicons.cpp, and modernui.cpp
- Mechanical tests: v2.5-beta-1-modern/tests/modern_icon_pipeline_test.py and windows_icon_integration_test.py

## Primary sources

- SVG 2 specification: https://www.w3.org/TR/SVG2/
- Windows iconography guidance: https://learn.microsoft.com/en-us/windows/apps/design/style/iconography/app-icon-design
- Windows LoadImage sizing behavior: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadimagew
- SDL3 window icon API: https://wiki.libsdl.org/SDL3/SDL_SetWindowIcon
- Freedesktop desktop-entry specification: https://specifications.freedesktop.org/desktop-entry-spec/latest/

Use platform documentation to validate container and runtime behavior. Use the repository manifest, original resources, and explicit art review to validate Comic Chat coverage and visual intent.
