# Project structure

ComicChat is a portable-first repository. The retired MFC/C++ application is
not vendored here.

| Path | Purpose |
| --- | --- |
| `src/` | Zig application, renderer, protocol, native platform backends, and tests |
| `src/client/` | Portable shell, dialogs, workspace, accessibility semantics, and file workflows |
| `src/comic/` | Comic layout, panel, figure, balloon, notification, and rules logic |
| `src/net/` and `src/proto/` | IRC/IRCX, TLS, session, and Comic Chat wire protocols |
| `src/assets/testdata/` | Runtime AVB/BGB characters and backdrops used by the application |
| `assets/reference/emotions/` | Runtime emotion-face bitmaps retained as content assets |
| `docs/` | Current product, protocol, release, and contributor documentation |
| `docs/UI_LIBRARY.md` | Shared desktop design tokens, components, interaction states, and visual checks |
| `legacy/docs/` | Historical Microsoft-reference audits and migration records; excluded from binary packages |
| `tools/` | Reproducible asset and font tooling |
| `third_party/onyx-server/` | Pinned Onyx source submodule; ComicChat imports it through the narrow `onyx_tls_root.zig` boundary |

## Releases

The published `comicchat-portable-2026-07-20.1` release provides x86_64 Windows,
Linux, FreeBSD, and OpenBSD binary packages, a source archive with the required
subset of the pinned Onyx TLS source expanded, and a SHA-256 manifest.
The source archive is the self-contained distribution artifact; a repository
checkout instead requires `git submodule update --init --recursive` before
building.

The external historical source reference is
<https://github.com/microsoft/comic-chat>. It informs behavior and provenance,
but it is not a second implementation lane in this repository.
