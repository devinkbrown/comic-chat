# Native C++26 port

`portable/` is the native Linux, FreeBSD, and OpenBSD port. It uses SDL3 for
windowing, input, audio initialization, clipboard access, and Wayland/X11
selection; Cairo for the software comic canvas; FreeType, HarfBuzz, and ICU for
Unicode text; zlib for bounded asset inflation; and one shared libuv + mbedTLS
connection engine also consumed by the modern Windows/MFC client. It does not
use Wine.

The code requires Clang 21 or newer in strict post-C++23 mode. Meson spells the
mode `cpp_std=c++26`; its Clang command line is `-std=c++2c` plus
`-pedantic-errors`, and `cpp26.hpp` rejects an older language mode. Clang 21 is
accepted for OpenBSD stable. The primary Linux CI gate is pinned to upstream
Clang 22.1.8 and rejects a different compiler version.

## Source-derived rendering foundation (incomplete)

The current renderer is a headless-testable title-panel foundation, not a
complete Comic Chat client or a visual-parity port. It uses measurements and
ordering rules read from Microsoft's released 2.5 beta source:

- `v2.5-beta-1-modern/defines.h` establishes 1,440 logical units per inch,
  `MINUNITPANELWIDTH`/`MINUNITPANELHEIGHT` at 2,300, and the original title and
  shout font heights.
- `panel.cpp` establishes two panels per row, 144-unit gutters, the 60-unit
  panel border, 500-unit star icons and rows, 100-unit icon/name spacing, and
  the 300/4860 below-`STARRING` offset.
- `CUnitPanelPage::AddTitle` makes the title panel borderless with no backdrop,
  lays its title into the upper half, then adds `STARRING` and the roster.
- `AddStarsAux` fixes roster order: the local user first, present users before
  departed users, then descending send count; users without an icon are not
  shown. `layout.cpp::order_stars` preserves those rules.
- `CUnitPanelPage::AddStars` centers the combined icon/name column and ellipsizes
  single-line names. `render.cpp` carries those logical constants into the
  portable title-panel demo and converts at the final canvas scale.
- `pageview.cpp::OnDraw` confirms the retained comic page is rendered in logical
  coordinates and transformed only for display/printing. The native canvas
  uses the same coordinate model and SDL only presents the completed ARGB frame.

The demo currently substitutes colored initial circles for roster art. It does
not yet port `CBodyUnary` avatar poses/body artwork, comic balloons, the expert
placement system, the full room/page/composer/member-list shell, or exact
`CLabel` font-bounding-box behavior. Those remain explicit port work; the
source-derived constants and title/roster tests should not be read as a claim
of finished visual fidelity.

The deterministic headless renderer can produce a review image without a
display server:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  ./build/comic-chat --frames 2 --png title-panel.png
```

## Build and run

Install Clang, Meson, Ninja, pkg-config, SDL3, Cairo, FreeType, HarfBuzz, ICU,
zlib, and libuv development packages. Then initialize the pinned native
dependencies and build:

```sh
git submodule update --init --recursive
cd portable
CC=clang CXX=clang++ meson setup build --buildtype=release
meson compile -C build
meson test -C build --print-errorlogs
./build/comic-chat
```

SDL selects Wayland automatically in a Wayland session. To make the backend an
explicit acceptance gate, run `SDL_VIDEODRIVER=wayland ./build/comic-chat`.
X11 remains an SDL fallback. FreeBSD and OpenBSD use the same Meson source; no
Linux-only frontend APIs are used.

System libraries are preferred. The checked-in `.wrap` descriptors provide
hash-pinned source fallbacks. As verified against the official releases on
2026-07-17, the direct source fallbacks are SDL 3.4.12, Cairo 1.18.4, FreeType
2.14.3, HarfBuzz 14.2.1, ICU 78.3, zlib 1.3.2, libuv 1.52.1, and Catch2 3.15.2.
The SDL and ICU archives use the newest applicable WrapDB Meson overlays
available on that date (SDL 3.4.2-1 and ICU 78.2-1). SDL's overlay is vendored
for review with its version metadata updated to 3.4.12, its Wayland source
list extended for the upstream `SDL_waylandutil.c` translation unit, and SDL's
upstream dummy audio/video backends enabled for headless tests; no SDL source
is modified. ICU's exact hash-pinned overlay is followed by a one-line
78.3 metadata patch. libuv is
pinned to upstream v1.52.1 commit
`1cfa32ff59c076ffb6ed735bbc8c18361558661f`; mbedTLS is pinned to upstream
v3.6.7 commit `068ff080b369adfac81509f9b57b2afabaf82dc5`. Downloaded `packagecache/`
archives and extracted subprojects are deliberately ignored. For an offline
machine, run `meson subprojects download` once on a connected cache-preparation
machine and transfer that local cache, or install the listed system packages;
do not commit the downloaded trees.

## Shared transport and IRCv3

`ConnectionEngine` is a typed, thread-safe command/event boundary. A dedicated
cooperatively stopped RAII thread owns the libuv loop and every socket/TLS
object. It maps directly to `std::jthread`/`std::stop_token` when the platform
library provides them and uses the same no-detach contract on BSD libc++
releases that do not. The UI thread
posts generation-tagged commands, wakes the loop through `uv_async_t`, and
polls immutable generation-tagged events. Socket readiness uses `uv_poll_t`, so
an idle connected client sleeps rather than checking the socket on a timer.

The connection path provides:

- interleaved IPv6/IPv4 resolution and 250 ms Happy Eyeballs attempts;
- TLS 1.2+ by default, SNI and hostname verification, optional CA-file override,
  Unix trust bundles, and the Windows ROOT certificate store;
- pinned mbedTLS session capture and best-effort resumption reporting, with a
  loopback cache test covering the observed resumed-session path;
- SOCKS5 (including username/password) and HTTP CONNECT tunnels;
- bounded receive, transmit, command, event, proxy-reply, and serialized-session
  buffers with explicit backpressure;
- weighted fair priority queues in `PONG`, authentication, control, chat, and
  bulk order, plus token buckets for chat and bulk traffic;
- connect, proxy/TLS handshake, and idle deadlines; cancellation by the
  portable stop token and generation; exponential reconnect with deterministic
  jitter; and zeroization of every sensitive send on completion or cancellation;
- diagnostics containing fixed non-secret descriptions only.

Plaintext is never selected implicitly: callers must set
`Security::plaintext`. STS parsing and persistence belong to the IRCv3 policy
engine, which emits the upgrade policy before transport options are created.
IRCv3 also owns CAP 302, SASL EXTERNAL/PLAIN/SCRAM-SHA-256, labels, batches,
multiline, history recovery, read markers, metadata, redaction, and safe
reconnect commands.

## Memory and concurrency limits

Defaults are 256 KiB each for receive and transmit buffering and 1,024 commands
or events. Proxy replies and serialized sessions are capped at 16 KiB and
64 KiB respectively. IRC line framing and zlib inflation require caller-supplied
limits. Nothing caches unbounded history, frames, proxy data, or wire data.

`FrameArena` is a bounded `std::pmr::monotonic_buffer_resource` with a null
upstream; exhaustion throws instead of growing the process. Render primitives
are compact contiguous values and finalized into immutable generation-tagged
snapshots. `LockedSecret` zeroizes credentials with `mlock`/`VirtualLock` and
requires the native page lock to succeed. `WorkerScheduler` uses one to
eight cooperatively stopped workers (bounded by reported hardware concurrency), a
bounded task queue, generation cancellation, and a deterministic inline mode
(`-Ddeterministic=true`) for goldens.

## Verification and performance

The normal suite includes Catch2 unit tests, the standalone IRCv3 protocol
suite, local plaintext/TLS loopback integration tests, hostname rejection,
handshake deadlines, cancellation, bounded backpressure, SOCKS5/HTTP CONNECT,
reconnect, TLS session resumption, and an idle no-spin assertion. The SDL dummy
driver is the headless frontend smoke gate.

Meson's built-in sanitizer option is supported without alternate source paths:

```sh
CC=clang CXX=clang++ meson setup build-asan \
  -Db_sanitize=address,undefined -Db_lundef=false
meson test -C build-asan --print-errorlogs

CC=clang CXX=clang++ meson setup build-tsan \
  -Db_sanitize=thread -Db_lundef=false -Dfrontend=false
meson test -C build-tsan --print-errorlogs
```

`comicchat-bench` reports repeatable nanoseconds-per-operation for source layout,
256-primitive PMR render batches, and deterministic task dispatch. Run it via:

```sh
meson test -C build --suite perf --verbose
```
