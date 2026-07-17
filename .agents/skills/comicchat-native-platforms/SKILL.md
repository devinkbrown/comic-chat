---
name: comicchat-native-platforms
description: Implement, review, and verify Comic Chat's native platform boundaries across Linux, Wayland, X11, FreeBSD, OpenBSD, and modern Windows/MFC. Use when changing SDL/Cairo presentation, OS services, compile guards, app identity, event loops, platform dependencies, Meson or nmake integration, cross-platform APIs, or platform-specific CI.
---

# Comic Chat native platforms

## Establish the platform contract

1. Read AGENTS.md, portable/README.md, and references/platform-matrix.md.
2. Identify every platform that consumes the changed API and every workflow job that proves it.
3. Separate portable core behavior from presentation, OS service, and native UI behavior.
4. Add the narrowest platform-independent test before adding platform-specific code.
5. Use comicchat-native-ui alongside this skill for dialogs, window chrome, focus, keyboard navigation, accessibility, DPI, or interactive UI behavior.

## Preserve architecture boundaries

- Keep portable/include/comicchat/ and portable/src/ free of Linux-only assumptions unless a focused implementation file and compile guard own them.
- Use SDL3 for portable window, input, clipboard, audio initialization, and Wayland/X11 selection. Use Cairo for the shared software comic canvas.
- Keep native Windows in the matching -modern/ MFC tree. Do not replace MFC/Win32 ownership with SDL, Wine, or a Unix abstraction.
- Keep shared transport and IRCv3 policy under portable/ and compile the same public API for the MFC consumer.
- Keep platform handles and headers out of shared value types. Hide them behind implementation ownership or a narrow adapter.
- Preserve the reverse-domain application identity io.github.devinkbrown.ComicChatReinked across SDL metadata, Wayland desktop integration, packaging, and generated icons.
- Prefer the pinned/system dependency policy already declared in portable/meson.build and wrap files. Do not add an unpinned download or silently substitute an older ABI.

## Implement Unix and BSD behavior

- Let SDL choose Wayland by default and retain X11 fallback. Force a backend explicitly when proving backend-specific behavior.
- Keep the render result identical across dummy, Wayland, and X11 presentation; presentation must not become a second renderer.
- Avoid direct Linux-only APIs in code required by FreeBSD or OpenBSD. Use libuv, SDL, POSIX, or a small guarded implementation with a tested fallback.
- Preserve OpenBSD's Clang 21 compatibility and thread_compat.hpp path. Do not use a standard-library facility merely because Linux Clang 22 provides it.
- Keep headless tests independent of a display and audio server.

## Implement native Windows behavior

- Preserve the x86 MSVC/MFC build contract in v1.0-pre-modern/ and v2.5-beta-1-modern/.
- Keep /std:c++latest, /permissive-, static runtime alignment, generated-resource includes, and the pinned static libuv/mbedTLS libraries intact.
- Keep UI-thread message pumping responsive while shared network work stays on its joinable owner thread.
- Use Win32 security, path, certificate-store, page-locking, and DPI APIs through focused adapters. Do not emulate Unix behavior when Windows provides the native contract.
- Retain package execution from a random directory with an unrelated working directory. Never assume the repository layout at runtime.

## Verify platform claims

Run the release build and headless suite first:

~~~sh
CC=clang CXX=clang++ meson setup <build-dir> portable --buildtype=release
meson compile -C <build-dir>
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  meson test -C <build-dir> --print-errorlogs
~~~

For Wayland behavior, launch under a real compositor and require a nonempty PNG, as .github/workflows/build-modern.yml does. For X11-specific behavior, force SDL_VIDEODRIVER=x11 under a real X server; the dummy driver is not X11 evidence.

Require the FreeBSD 15, OpenBSD 7.9, and native Windows jobs on the exact integrated commit whenever shared platform-facing code changes. Run the native nmake build and random-folder package smoke on Windows when available.

Treat compile-only checks as compile evidence, not runtime evidence. Report the exact platform, compiler, backend, command, outcome, skipped surfaces, and remaining platform-specific risk.
