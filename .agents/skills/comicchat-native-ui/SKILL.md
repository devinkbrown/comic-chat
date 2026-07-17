---
name: comicchat-native-ui
description: Modernize, review, and verify Comic Chat's native desktop windows, dialogs, chrome, DPI behavior, keyboard/focus interaction, and accessibility while preserving original resource semantics. Use when changing MFC dialog resources/classes, title bars, close/minimize/resize behavior, property sheets, modal parenting, SDL/Wayland top-level windows, focus order, keyboard shortcuts, high-contrast behavior, or multi-DPI visual layout.
---

# Comic Chat native UI

## Establish the UI oracle

1. Read AGENTS.md, docs/MODERNIZATION.md, docs/dpi-awareness.md, and references/ui-contract.md.
2. Compare the exact dialog/resource and owning class in v2.5-beta-1/ with its v2.5-beta-1-modern/ counterpart.
3. Classify the surface as a top-level window, modal dialog, modeless tool window, property sheet, property page/child, or portable SDL top level.
4. Preserve the original controls, labels, commands, defaults, and workflow unless the task explicitly changes product behavior.
5. Capture baseline screenshots and keyboard behavior before editing.

Use comicchat-icon-assets for icon masters/generation, comicchat-render-fidelity for the comic canvas, and comicchat-native-platforms for OS/build integration.

## Prefer real native chrome

- Create decorated, resizable native top-level windows and dialogs when content can grow or users keep them open. Keep a fixed dialog only with a concrete source or usability reason.
- Use the platform title bar, close/minimize controls, resize borders, system menu, activation state, and window manager policy. Do not paint fake caption buttons over a borderless content window.
- Keep WS_CAPTION, WS_SYSMENU, and WS_MINIMIZEBOX on eligible MFC top levels. Add WS_THICKFRAME and a DPI-aware minimum size where resizing is supported.
- Keep WS_CHILD property pages and embedded views free of top-level chrome. Give the containing property sheet the native chrome.
- Remove incompatible context-help extended chrome only where the native caption contract requires it; retain page-level help through accessible controls or F1.
- Preserve native close through the caption button, system menu, and Alt+F4. Preserve Escape as cancel and Enter as the default action where the original dialog contract allows them.
- Set correct owner/parent and modality. Do not allow an orphan dialog to hide behind its parent or steal application-wide focus.

## Scale layout dynamically

- Prefer Per-Monitor v2 behavior for modern Windows. Handle WM_DPICHANGED, use the suggested rectangle, query the target window DPI, and rebuild all DPI-sensitive metrics.
- Derive control sizes, padding, fonts, icons, minimum sizes, splitters, and hit targets from current DPI. Do not cache a process-start DPI forever.
- Preserve the comic's TWIP coordinate model separately from pixel-based native chrome and controls.
- On SDL, request high-density, decorated, resizable windows with explicit title, application identity, parent, and modal properties as applicable.
- On Wayland, let the compositor or libdecor own decorations. Respect xdg-shell configure/acknowledge, parent, minimum/maximum size, and close contracts.
- Test 96 DPI (100%), 144 DPI (150%), and 192 DPI (200%), plus a live move between different-DPI displays. Check startup at each scale and runtime transitions.

## Preserve keyboard and accessibility

- Define a logical initial focus and Tab/Shift+Tab order. Keep radio groups, lists, edits, property tabs, default buttons, cancel buttons, and mnemonics reachable without a pointer.
- Keep visible focus indicators, enabled/disabled state, high-contrast colors, text scaling, and sufficient unclipped labels at every required DPI.
- Associate labels with editable controls and expose meaningful accessible names, roles, state, and values through native controls.
- Do not encode meaning only through color, icon shape, hover, or custom drawing.
- Preserve screen-reader and keyboard behavior when replacing a legacy control. Do not claim accessibility from visual similarity.
- Test close, minimize, restore, resize, focus restoration, default/cancel, Alt+F4, Escape, Enter, tab traversal, and relevant arrow-key navigation.

## Verify native behavior

Run the resource-level audit:

~~~sh
python3 v2.5-beta-1-modern/tests/dialog_chrome_test.py
~~~

Run icon/integration checks when caption, dialog, toolbar, or control imagery changes. Build the native Windows client and smoke it from the packaged random path.

For portable windows, run the headless suite and a real Wayland compositor smoke. A dummy SDL window proves construction, not compositor decorations, focus, resizing, or accessibility.

Capture Windows screenshots at 100%, 150%, and 200% and Wayland screenshots with the active compositor decoration policy. Record window style/properties, keyboard results, focus order, clipped controls, high-contrast result, and any platform limitation. Add a deterministic resource or state test for every regression that can be checked without visual judgment.

Report the original resource/source oracle, modern files changed, window role, DPI matrix, keyboard/accessibility evidence, native smoke result, and any intentional divergence.
