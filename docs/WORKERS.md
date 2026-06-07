# Worker brief — comicchat (pure-Zig Microsoft Comic Chat)

Pure Zig 0.16, **no C, no SDL, no external deps**. Reimplements Microsoft Comic
Chat: decode its `.bgb` backgrounds and `.avb` avatars, assemble characters, and
render comic panels. Build: `zig build`  ·  Test: `zig build test`  ·  Run:
`./zig-out/bin/comicchat <subcommand>`.

## Hard rules
- **Pure Zig only.** No `@cImport`, no linking C libs, no new dependencies.
- Add your feature in your **own new file(s)** under `src/`. You may add a line
  to `src/root.zig` (module registry) and a subcommand to `src/main.zig`, but
  keep edits there minimal — the integrator resolves conflicts.
- Every new module must have inline `test "..."` blocks and pass `zig build test`.
- Match the existing style (see neighbouring files). Small functions, comments.
- The original MS assets live in `src/assets/testdata/*.avb` / `*.bgb`
  (git-ignored, present locally). Use `@embedFile` for tests.

## Zig 0.16 gotchas (you WILL hit these)
- `std.ArrayList(T)` is **unmanaged**: `var l: std.ArrayList(u8) = .empty;`
  then `try l.append(gpa, x)` / `l.appendSlice(gpa, s)` / `l.deinit(gpa)`.
- No `std.mem.trimRight` → `std.mem.trimEnd` (also `trimStart`, `trim`).
- No `GeneralPurposeAllocator` → use `std.heap.page_allocator` (CLI) or
  `std.testing.allocator` (tests); `DebugAllocator` exists too.
- Networking is under `std.Io.net` (no `std.net`); needs a `std.Io.Threaded`
  runtime — see `src/net/transport.zig`.
- File/stdout: there is **no `std.posix.write`/`std.os.argv`**. `main` takes
  `std.process.Init.Minimal` (see main.zig); write bytes via
  `std.os.linux.write(fd, ptr, len)` (Linux) — see `writeAllFd` in main.zig.
- `@abs(i32diff)` is unsigned → `@as(i32,@intCast(@abs(...)))`. Signed division
  needs `@divTrunc`/`@divFloor`.
- Tests aggregate via `_ = @import("yourfile.zig");` in root.zig's test block.
- Inflate: `var r: std.Io.Reader = .fixed(bytes); var w:[std.compress.flate.max_window_len]u8=undefined; var d=std.compress.flate.Decompress.init(&r,.zlib,&w); try d.reader.readSliceAll(out);` (known size) or `streamRemaining(&aw.writer)` into `std.Io.Writer.Allocating`.

## Public API you build on
- `cc = @import("comicchat")` (the library; this is `src/root.zig`).
- `cc.assets.bgb.Image` = `{ width:u32, height:u32, pixels:[]u32 }` (0xAARRGGBB,
  top-down; alpha 0 = transparent). `img.deinit(gpa)`.
- `cc.assets.bgb.decodeBackground(gpa, bytes) !Image` — 315×315 scene.
- `cc.assets.bgb.decodePoseAuto(gpa, bytes, index, tall) !Image` — one pose.
- `cc.comic.figure.assemble(gpa, avb, emotion, gesture) !Image` — full character
  on a **transparent** background (white sticker + black ink). Use this to place
  a character in a panel.
- `cc.comic.figure.composite(canvas, src, sw, sh, dx, dy, crop_r)` — blit a
  transparent-keyed image (opaque, upper layer occludes).
- `cc.render.canvas.Canvas` — RGBA framebuffer: `init/deinit/clear/fillRect/blit/
  blitScaled/blitScaledAlpha/drawLine/fillTriangle/drawText/drawTextWrapped/
  wrappedHeight/speechBalloon`. Colors `cc.render.canvas.black`/`white`.
- `cc.net.client.Client` (connect/register/join/privmsg/next), `cc.net.message`
  (RFC1459 parse/write), `cc.proto.record` (comic tagged-record codec).
- Avatars: anna armando bolo cro dan denise hugh jordan kevin kwensa lance lynnea
  margaret maynard mike rebecca sage scotty susan tiki tongtyed xeno.
- Backgrounds: field volcano den room pastoral. (`avatarByName`/`bgByName` in main.zig.)

## Emitting images
`emitPpm(gpa, pixels, w, h)` in main.zig writes a binary PPM (P6) to stdout.
A pure-Zig PNG encoder (`src/render/png.zig`) is being added — prefer it when present.
