# comicchat

A modern, cross-platform reimplementation of **Microsoft Comic Chat 2.5** —
in **pure Zig** (0.16). No SDL, no C interop, no external dependencies.

Comic Chat (1996) turned IRC conversations into auto-generated comic strips:
each participant picks an avatar, and the client composes panels with speech
balloons, poses, and emotions. The comic state rides inside ordinary IRC
messages as tagged text, so it interoperates with any IRC server.

This project rebuilds it from scratch, informed by a full Ghidra decompilation
of the original `cchat.exe` (see [`docs/PROTOCOL.md`](docs/PROTOCOL.md)).

## Status

Early. The platform-independent core is underway; the windowing/render layer
(hand-rolled per OS + software rasterizer, since GPU APIs are C) comes later.

| Module            | Path                  | State                          |
|-------------------|-----------------------|--------------------------------|
| Protocol codec    | `src/proto/`          | ✅ parse/encode + tests        |
| Asset decoders    | `src/assets/`         | ⏳ `.avb`/`.bgb` (`0x8181`)    |
| IRC client        | `src/net/`            | ⏳ RFC1459 + IRCX over std.net |
| Comic auto-layout | `src/comic/`          | ⏳ from SIGGRAPH '96 paper     |
| Software renderer | `src/render/`         | ⏳ pure-Zig rasterizer         |
| Window / input    | `src/ui/`, per-OS     | ⏳ Wayland/X11, Win32, Cocoa   |

## Build

Requires Zig 0.16+.

```sh
zig build test     # run unit tests
zig build run      # headless demo: encode then decode a comic transcript
zig build          # build the executable
```

Cross-compile (Zig's headline feature):

```sh
zig build -Dtarget=x86_64-windows
zig build -Dtarget=aarch64-macos
zig build -Dtarget=x86_64-linux
```

## Design tenets

- **Pure Zig.** The only OS-specific code is the windowing backend, which talks
  to each platform's native API directly (Wayland/X11 wire protocol, Win32
  `extern`, Cocoa) — no C libraries linked.
- **Interoperable.** Comic metadata is backward-compatible with plain IRC; a
  non-comic client just sees text.
- **Faithful then modern.** Match the original's behavior first (decompilation
  is the reference), then improve.

## Legal

Clean-room reimplementation. Original Microsoft artwork/assets are copyrighted
and are not redistributed here.
