# Worker brief — Comic Chat

This repository has two intentionally separate implementation lanes:

- `src/` is the portable client. It uses one software framebuffer renderer
  with direct X11, Wayland, and Win32 backends and no SDL. Its sole linked C
  dependency is the pinned official mbedTLS 3.6.6 TLS implementation.
- `legacy/` is the byte-verified Microsoft Chat 2.5 beta 1 MFC source plus
  modern Windows build/package wrappers. It requires Visual Studio 2022, MFC,
  and an x86 Windows toolchain.

The portable tree is currently tested with Zig
`0.17.0-dev.1282+c0f9b51d8`. Its standard gates are:

```sh
zig build test
zig build
zig build -Dtarget=x86_64-windows
zig build -Dtarget=x86-windows
zig build -Dtarget=aarch64-windows
```

## Rendering source of truth

Microsoft's repository at <https://github.com/microsoft/comic-chat>, pinned by
`legacy/PROVENANCE.md`, is the behavioral source of truth. Do not introduce a
second heuristic layout or balloon path. The source-derived portable pipeline
is split across:

- `src/comic/original_page.zig`: AddLine/AddReaction, title accounting,
  retries, clones, continuations, and the shared random stream.
- `src/comic/original_layout.zig`: avatar ordering, geometry, and talk-to
  relationships.
- `src/comic/original_balloon.zig`: source line breaking, placement, tails,
  thought/action shapes, and whisper dashes.
- `src/comic/original_figure.zig`: AVB component selection, masks, ROPs, and
  logical-coordinate figure placement.
- `src/comic/original_title.zig` and `original_raster.zig`: title layout and
  the 2300-logical-unit-to-315-pixel software rasterizer.
- `src/comic/strip.zig`: public transcript-to-page integration. All shipped
  strip and panel rendering must route through this source pipeline.

Fixed source-parity contracts live in `src/comic/source_*_test.zig`. When a
deliberate source-derived raster change updates a golden hash, record the
upstream routine that justifies the change rather than merely refreshing the
hash.

## Platform boundaries

- Portable core code under `assets/`, `comic/`, `net/`, `proto/`, and
  `render/` must remain independent of the window system.
- `src/platform/x11.zig` and `wayland.zig` implement the native Linux paths.
  A nonempty `WAYLAND_DISPLAY` selects Wayland; there is no automatic X11
  fallback after a Wayland connection failure.
- `src/platform/win32.zig` uses direct Win32 declarations and presents the
  same software framebuffer. It is not the MFC legacy lane.
- `src/client/` owns portable view/input behavior shared by native backends.
- The direct Wayland keyboard path is currently US evdev only; do not claim
  XKB, compose, IME, or key-repeat support until implemented and tested.

## Change rules

- Keep rendering and platform presentation in Zig with no SDL. mbedTLS at the
  exact `build.zig.zon` revision is the deliberate transport exception; do not
  replace it with an unpinned system library or weaken certificate checks.
- Add focused inline tests and aggregate a new test-only module from
  `src/root.zig` when necessary.
- Preserve the imported paths enumerated in `legacy/PROVENANCE.md`. Verify
  them with `legacy/scripts/verify-import.sh <upstream-checkout>`; put build or
  packaging adaptations outside those paths.
- Do not add or redistribute AVB/BGB files without an exact source path,
  checksum, and applicable license. The current portable asset audit and Xeno
  import procedure are in `PORTABLE_ASSET_PROVENANCE.md`.
- Keep generated font changes reproducible through `tools/generate_font.py`
  and retain `src/render/COMIC_NEUE_LICENSE.txt`.
- Preserve unrelated working-tree changes. Use `zig fmt` for Zig edits and
  finish with `zig build test` plus `git diff --check`.

## Useful APIs

- `cc = @import("comicchat")` imports `src/root.zig`.
- `cc.assets.avb` and `cc.assets.bgb` parse source AVB/BGB records and images.
- `cc.comic.strip.render` / `renderWithOptions` produce source-layout pages.
- `cc.comic.original_figure.drawForTextLogical` draws authored AVB layers
  using source logical geometry.
- `cc.render.canvas.Canvas` is the shared RGBA software framebuffer.
- `cc.net.client.Client` provides IRC connect/register/join/message behavior;
  `cc.proto.record` handles Comic Chat tagged records.

The portable IRC transport defaults to verified TLS and port 6697. Plaintext
exists only behind the explicit `--plaintext` compatibility flag; there is no
automatic insecure fallback.

## Modern portable network boundary

Keep `src/net/` ownership-oriented and transport-independent above the socket:

- `message.zig` and `irc.zig` are immutable parse views plus bounded framing.
- `ircv3.zig` and `sasl.zig` are pure typed registration state machines.
- `features.zig` owns negotiated identity, ISUPPORT, label, echo, redaction,
  metadata, and nested BATCH state.
- `connection_policy.zig` owns bounded priority/backpressure queues, token
  buckets, deadlines, reconnect jitter, safe restoration, and proxy codecs.
- `transport.zig` owns the joinable async connector, bounded IPv6/IPv4 address
  race, real SOCKS5/HTTP CONNECT handshake, and winning-socket TLS handoff.
- `client.zig` is the only live composition layer; it advances registration
  from receive events and never waits synchronously for a CAP or SASL reply.
- `sts_store.zig` owns the bounded host policy database and atomic persistence.

The native app opens its window before connection setup. `AsyncNetwork` polls
the connector, performs first-contact STS TLS upgrades, and schedules jittered
reconnects; platform event loops must never call the blocking compatibility
`Client.connectWithOptions` path.

Do not introduce mutable protocol globals, detached worker ownership, raw
credential logging, unbounded receive collections, or direct UI-to-socket
serialization. Parsed messages borrow framing storage; copy only the fields
that must survive the next receive into an owning bounded structure. Rendering
and comic-layout code must not depend on network transport details.
