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
| `docs/` | Product, protocol, provenance, and migration documentation |
| `tools/` | Reproducible asset and font tooling |

The external historical source reference is
<https://github.com/microsoft/comic-chat>. It informs behavior and provenance,
but it is not a second implementation lane in this repository.
