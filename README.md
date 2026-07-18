# comicchat

A source-faithful continuation of **Microsoft Comic Chat 2.5**, developed in
two coordinated lanes:

- `src/` is the new portable client, with a Zig software renderer and native
  platform backends plus the pinned official mbedTLS transport.
- [`legacy/`](legacy/README.md) preserves Microsoft Chat 2.5 beta 1 as a Win32
  MFC application that builds with a current Visual Studio toolchain.

Comic Chat turns IRC conversations into auto-generated comic strips. Each
participant has an avatar, and the client composes panels with speech balloons,
poses, and emotions while remaining interoperable with ordinary IRC clients.

The rendering reference is Microsoft's open-source Comic Chat repository:
<https://github.com/microsoft/comic-chat>. The portable implementation ports
the original panel, avatar-placement, balloon, and emotion behavior from that
source instead of approximating it from screenshots. The legacy import is
pinned to revision `c7df00f60bc8e9fdef413f139e61f7c37e024684`.

The portable page keeps the released client's visible contract: an implicit
borderless title/starring panel, 2300×2300 logical conversation panels, two
panels per row, 144-unit interstices, authored AVB icons and mask layers, and
source-seeded panel/balloon layout. Comic Neue Bold and Bold Italic are bundled
as the SIL-OFL portable substitutes for the proprietary Comic Sans MS faces
requested by the Windows client; the legacy Windows lane uses the original GDI
font path. As in Microsoft's source, only Woodring whisper balloons select the
italic face.

## Project layout

| Lane | Path | Purpose |
| --- | --- | --- |
| Portable client | `src/` | Zig IRC client, AVB/BGB decoding, original rendering behavior, software rasterizer, and native X11/Wayland/Win32 presentation |
| Legacy Windows client | `legacy/` | Byte-identical import of Microsoft's `v2.5-beta-1-modern` MFC source plus reproducible build, package, provenance, and smoke-test wrappers |
| Protocol notes | `docs/PROTOCOL.md` | Comic Chat wire-format and interoperability notes |

## Portable Zig client

The active tree is tested with Zig `0.17.0-dev.1282+c0f9b51d8`:

```sh
zig build test
zig build
zig build run                         # record-codec demo
zig build run -- render-strip > comic.ppm
zig build run -- window anna          # native window/backend smoke
zig build run -- app irc.example nick '#channel'              # verified TLS, port 6697
zig build run -- app irc.example 6697 nick '#channel' --ca-file ./ca.pem
zig build run -- app irc.example nick '#channel' --socks5 127.0.0.1:1080
zig build run -- app irc.example nick '#channel' --http-proxy proxy.example:8080
zig build run -- app localhost 6667 nick '#channel' --plaintext
```

The app opens before DNS/TCP/TLS setup and keeps the native event loop live
while a bounded connector races IPv6/IPv4 candidates. `--connect-timeout-ms`
sets the per-address and proxy-read deadline. SOCKS5 uses no-auth remote-DNS
CONNECT; HTTP proxies use a bounded CONNECT response. TLS hostname and chain
verification still target the IRC host after either proxy handshake.

### Regenerating the portable font atlas

The generated atlas is reproducible from
[Comic Neue](https://github.com/crozynski/comicneue) commit
`ef5be72411141d01f0b865df8edb47e552c11c3c`. With Python and Pillow installed,
pass that revision's `ComicNeue-Bold.ttf` and `ComicNeue-BoldItalic.ttf` to the
generator. Pillow is pinned because its bundled FreeType rasterizer is part of
the byte-exact atlas toolchain:

```sh
python3 -m pip install -r tools/font-requirements.txt
python3 tools/generate_font.py \
  /path/to/ComicNeue-Bold.ttf \
  /path/to/ComicNeue-BoldItalic.ttf
```

The generator rejects inputs unless their SHA-256 values are respectively
`3e7e5fccfd7e0788f317b43312151c1bd5cf058c9697a8d83eac3939050bd61e`
and
`5c312c2a2fa64eee82f3b87fcfab8f3b12a5e59b043124401d322eb323cfbf16`.
It also rejects rasterizer drift before rewriting `font.zig`/`fontdata.bin` and
`font_italic.zig`/`fontdata_italic.bin`. The SIL Open Font License covering both
faces is retained in `src/render/COMIC_NEUE_LICENSE.txt`.

Cross-compile examples:

```sh
zig build -Dtarget=x86_64-windows
zig build -Dtarget=x86-windows
zig build -Dtarget=aarch64-windows
zig build -Dtarget=x86_64-linux
```

Cross-compilation installs the Windows binary at
`zig-out\bin\comicchat.exe`; it does not execute it. On Linux, a nonempty
`WAYLAND_DISPLAY` selects the Wayland backend and an unset/empty value selects
X11. There is no automatic fallback after a Wayland connection failure. To
force the X11 smoke explicitly:

```sh
env -u WAYLAND_DISPLAY zig build run -- window anna
```

The direct Wayland client currently uses scale 1 and a US evdev key map; it does
not yet negotiate output scale or provide XKB layout/compose, IME, or key-repeat
support. Win32 is system-DPI aware rather than per-monitor-v2 aware. Window
creation, configure/resize, shared-memory presentation, keyboard input, IRC
traffic, and clean close are implemented on both Wayland and Win32. Pointer
interaction is not part of the portable client UI yet.

The portable lane has no SDL dependency. Native backends speak the Wayland/X11
protocols or Win32 APIs directly, and all display the same software-rendered
comic framebuffer. IRC connections use verified TLS by default on port 6697,
through official mbedTLS 3.6.6 sources pinned in `build.zig.zon`. The client
requires a trusted certificate, sends SNI, verifies the requested hostname,
and never falls back to plaintext. It loads the Windows ROOT certificate store
or common Unix CA bundles; `--ca-file <pem>` overrides those roots.
`--plaintext` is an explicit compatibility mode for trusted local servers that
do not offer TLS and must not be used for credentials on untrusted networks.

Live messages use the released compact UDI grammar: the portable client reads
both embedded non-IRCX annotations and IRCX `DATA ... CCUDI1` state; preserves
the authored face/torso ordinals, emotion, intensity, requested-pose flag,
balloon mode, and talk-to participants; and renders that cooked AVB state. For
outgoing text it runs the source-derived pose rules, sends standalone `DATA`
metadata after IRCX negotiation, and otherwise uses the original embedded
annotation form. Ordinary IRC clients still receive readable message text.
The client also consumes the source `# Appears as ...` avatar control, announces
its current bundled avatar after joining, and supports `/avatar <name>` in the
interactive input so later panels use the selected character.

## Legacy Windows client

The faithful old-client port targets Windows x86 with Visual Studio 2022 and
MFC. From a Windows command prompt:

```bat
cd legacy
build.cmd Release --clean
```

On any Unix-like development host, verify the imported snapshot and its exact
Microsoft Git blobs with:

```sh
cd legacy
./scripts/verify-import.sh /path/to/microsoft-comic-chat
```

See [`legacy/README.md`](legacy/README.md) for Windows prerequisites, packaging,
runtime smoke testing, and known limitations. The legacy IRC transport is
plaintext; use a trusted local TLS tunnel or bouncer and never send sensitive
credentials over an untrusted network.

After a Windows Release build, package and exercise the isolated archive with:

```powershell
pwsh -NoProfile -File .\scripts\package.ps1
pwsh -NoProfile -File .\scripts\smoke.ps1
```

## Design tenets

- **Source-faithful rendering.** Microsoft's original implementation is the
  behavioral source of truth for panel splitting, avatar order and scale,
  emotion selection, text measurement, balloon routing, and tails.
- **One portable core.** Protocol, assets, layout, rendering, and client state
  are platform-independent; native backends own window/event integration and
  framebuffer presentation.
- **Interoperable IRC.** Comic metadata remains compatible with ordinary IRC;
  clients without Comic Chat still see the conversation text.
- **Preserve the original.** The MFC application remains available as a
  separately buildable, integrity-verifiable Windows lane.

## License and provenance

Microsoft's upstream repository is published under the MIT License. The legacy
snapshot retains that license, the upstream source, and its bundled AVB/BGB art
verbatim; see [`legacy/PROVENANCE.md`](legacy/PROVENANCE.md) and
[`legacy/NOTICE.txt`](legacy/NOTICE.txt). Microsoft names, logos, and artwork
may be trademarks, and builds from this repository are unofficial and
unsupported. The portable asset set has a byte-level source and transformation
audit in [`docs/PORTABLE_ASSET_PROVENANCE.md`](docs/PORTABLE_ASSET_PROVENANCE.md),
including the checksum-guarded Xeno import from the pinned Microsoft revision.
The generated portable font atlases are derived from Comic Neue Bold and Bold
Italic under the SIL Open Font License; see
[`src/render/COMIC_NEUE_LICENSE.txt`](src/render/COMIC_NEUE_LICENSE.txt).
The fetched mbedTLS 3.6.6 dependency is used under its Apache License 2.0
option; its license text remains in the pinned upstream package.
