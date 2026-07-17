# Native UI contract

## Repository oracle

| Concern | Immutable source | Modern implementation/evidence |
| --- | --- | --- |
| Dialog controls, IDs, labels, tab stops, default/cancel buttons | v2.5-beta-1/chat.rc and resource.h | v2.5-beta-1-modern/chat.rc and resource.h |
| Dialog and property-sheet ownership | v2.5-beta-1/chicdial.cpp, chicdial.h, and each owning class | v2.5-beta-1-modern/chicdial.cpp and owning classes |
| Window command behavior | Original message maps and OnOK/OnCancel/PreTranslateMessage handlers | Matching modern handlers |
| High-DPI lessons | Original drawing/source behavior | docs/dpi-awareness.md, modernui.cpp/.h, mainfrm.cpp, tabbar.cpp, saywnd.cpp |
| Modern Windows scope | Historical source and resources | docs/MODERNIZATION.md |
| Top-level chrome inventory | Original resource roles | v2.5-beta-1-modern/tests/dialog_chrome_test.py |
| Portable window | Product workflow and platform contract | portable/src/app.cpp and portable/packaging/linux/ |

Do not infer that every DIALOG or DIALOGEX block is a top level. Exclude WS_CHILD property pages and embedded views from top-level caption requirements, then audit their containing sheet/window.

## Windows top-level checklist

- Retain native caption, system menu, close, minimize, activation, drag, and Alt+F4 behavior.
- Prefer a resize border and DPI-aware minimum size for persistent or dynamic-content windows.
- Keep caption controls fully visible and let Windows handle their high-contrast, hover, pressed, active, and inactive states.
- Use PMv2, WM_DPICHANGED's suggested rectangle, GetDpiForWindow, and DPI-aware metrics.
- Recreate fonts and correctly sized image resources after DPI changes.
- Preserve a usable window when saved geometry came from another DPI, monitor layout, or remote session.
- Verify logical tab order, mnemonics, default button, Escape/cancel, accessible names, focus cues, and screen-reader state.

## SDL and Wayland top-level checklist

- Use SDL_CreateWindowWithProperties for title, resizable/decorated/high-density flags, parent, and modal relationship.
- Set the stable application ID and desktop metadata used by portable/packaging/linux/.
- Treat an xdg_toplevel as a platform-owned top level. Set title, app ID, parent, and size constraints; obey configure and close events.
- Negotiate xdg-decoration when exposed and accept compositor-side policy. Use libdecor when SDL requires client-side decorations.
- Do not implement custom close/minimize buttons merely to make Wayland and Windows look identical.
- Verify focus, keyboard, resize, close, compositor decoration, scale, and restoration under a real compositor.

## Required visual matrix

| Windows scale | Effective DPI | Check |
| --- | --- | --- |
| 100% | 96 | Original dimensions remain usable; no gratuitous enlargement |
| 150% | 144 | Text, controls, icons, hit targets, and title bar remain crisp and unclipped |
| 200% | 192 | Dialog fits or scrolls/resizes; no overlap, missing caption controls, or tiny pixel assets |

Also move a live window between different-DPI displays and verify focus, geometry, font/icon refresh, and no recursive resize loop.

## Primary current sources

- Windows title-bar behavior and caption controls: https://learn.microsoft.com/en-us/windows/apps/design/basics/titlebar-design
- Windows Per-Monitor v2 desktop guidance: https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows
- Windows accessibility overview: https://learn.microsoft.com/en-us/windows/apps/design/accessibility/accessibility-overview
- UI Automation for Win32: https://learn.microsoft.com/en-us/windows/win32/winauto/entry-uiauto-win32
- SDL3 window property creation: https://wiki.libsdl.org/SDL3/SDL_CreateWindowWithProperties
- Wayland xdg-shell: https://wayland.app/protocols/xdg-shell
- Wayland xdg-decoration: https://wayland.app/protocols/xdg-decoration-unstable-v1

Use platform guidance for native behavior and accessibility. Use Microsoft's immutable Comic Chat resources and handlers for product semantics and control fidelity.
