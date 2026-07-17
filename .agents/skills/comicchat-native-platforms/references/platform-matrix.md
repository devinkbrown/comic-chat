# Native platform matrix

## Required lanes

| Lane | Implementation | Toolchain | Runtime proof |
| --- | --- | --- | --- |
| Linux core/headless | portable/ | Pinned Clang 22.1.8, strict C++26 | Complete Meson suite with SDL dummy |
| Linux Wayland | portable/src/app.cpp through SDL3 | Same Linux build | Weston headless compositor, forced wayland backend, PNG output |
| Linux X11 | SDL3 fallback | Supported Clang build | Force x11 under a real X server when affected |
| FreeBSD 15 | Same portable/ sources | Clang 22 | Native VM build, tests, and PNG smoke |
| OpenBSD 7.9 | Same portable/ sources | Clang 21 | Native VM build, tests, and PNG smoke |
| Windows/MFC | v1.0-pre-modern/ and v2.5-beta-1-modern/ | MSVC 14.51+, /std:c++latest, x86 MFC | Build, package, random-folder launch |

The required CI definitions live in .github/workflows/build-modern.yml. Inspect that file rather than relying on job names remembered from an older revision.

## Ownership map

- Portable build and platform dependencies: portable/meson.build and portable/meson_options.txt
- Portable application/event loop: portable/src/app.cpp
- Shared rendering: portable/src/render.cpp
- Shared OS-sensitive memory and crypto: portable/src/memory.cpp and portable/src/crypto_runtime.cpp
- Shared network loop: portable/src/net/connection_engine.cpp
- BSD standard-library compatibility: portable/include/comicchat/thread_compat.hpp
- Linux desktop metadata: portable/packaging/linux/
- Windows build and resources: v2.5-beta-1-modern/chat.mak, chat.rc, modernui.cpp, and modernicons.cpp
- Packaging and random-folder smoke: scripts/package-modern-builds.ps1 and scripts/smoke-test-modern-builds.ps1

## Primary sources

- SDL3 API index: https://wiki.libsdl.org/SDL3/FrontPage
- SDL3 Wayland guidance: https://wiki.libsdl.org/SDL3/README-wayland
- SDL3 video-driver selection: https://wiki.libsdl.org/SDL3/SDL_HINT_VIDEO_DRIVER
- Wayland protocol and client API: https://wayland.freedesktop.org/docs/html/
- FreeBSD developer documentation: https://docs.freebsd.org/en/books/developers-handbook/
- OpenBSD ports and packages FAQ: https://www.openbsd.org/faq/ports/
- Windows desktop API: https://learn.microsoft.com/en-us/windows/win32/
- Windows high-DPI desktop guidance: https://learn.microsoft.com/en-us/windows/win32/hidpi/high-dpi-desktop-application-development-on-windows

Use current platform documentation for OS behavior. Use repository source and CI for the supported version, compiler, dependency, and packaging contract.
