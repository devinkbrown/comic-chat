# comicchat

A portable, source-faithful continuation of **Comic Chat**, built in Zig with
a software renderer, native X11/Wayland/Win32 presentation, and verified TLS.

Comic Chat turns IRC conversations into auto-generated comic strips. Each
participant has an avatar, and the client composes panels with speech balloons,
poses, and emotions while remaining interoperable with ordinary IRC clients.

The rendering reference is Microsoft's open-source Comic Chat repository:
<https://github.com/microsoft/comic-chat>. The portable implementation ports
the original panel, avatar-placement, balloon, and emotion behavior from that
source instead of approximating it from screenshots. The external historical
reference is pinned to revision `c7df00f60bc8e9fdef413f139e61f7c37e024684`.

The portable page keeps the released client's visible contract: an implicit
borderless title/starring panel, 2300×2300 logical conversation panels, two
panels per row, 144-unit interstices, authored AVB icons and mask layers, and
source-seeded panel/balloon layout. Comic Neue Bold and Bold Italic are bundled
as the SIL-OFL portable substitutes for the proprietary Comic Sans MS faces
requested by the historical Windows client. As in Microsoft's source, only
Woodring whisper balloons select the
italic face.

## Project layout

| Area | Path | Purpose |
| --- | --- | --- |
| Portable client | `src/` | Zig IRC client, AVB/BGB decoding, original rendering behavior, software rasterizer, and native X11/Wayland/Win32 presentation |
| Runtime assets | `assets/` and `src/assets/testdata/` | Attributed character, backdrop, and emotion content required by the portable renderer |
| Protocol notes | `docs/PROTOCOL.md` | Comic Chat wire-format and interoperability notes |
| Completeness audit | `docs/PORTABLE_COMPLETENESS_AUDIT.md` | Reachable, substrate-only, partial, and missing portable product surfaces |
| Repository map | `docs/PROJECT_STRUCTURE.md` | Portable-first repository ownership and layout |
| Historical reference | `legacy/docs/` | Microsoft-source audit, source-parity plan, and asset provenance record; not packaged in binary releases |

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
zig build run -- app your-nick                          # eshmaki.me, #root
zig build run -- app eshmaki.me your-nick '#root' \
  --tls-cert ./client-cert-and-key.pem --sasl-user your-account --sasl-external
```

On Windows, double-clicking `comicchat.exe` opens the desktop client directly
with the configured `eshmaki.me`, `comicchat`, and `#root` defaults. Use
`comicchat.exe app <nick>` or the full command form above to override them.

The app opens before DNS/TCP/TLS setup and keeps the native event loop live
while a bounded connector races IPv6/IPv4 candidates. `--connect-timeout-ms`
sets the per-address and proxy-read deadline. SOCKS5 uses no-auth remote-DNS
CONNECT; HTTP proxies use a bounded CONNECT response. TLS hostname and chain
verification still target the IRC host after either proxy handshake.

On Onyx Server, authenticated clients persist reusable `SESSION TOKEN` and
portable `SESSION MTOKEN` credentials in `.comicchat-session` (override with
`--session-file`). After SASL succeeds, reconnects prefer the unexpired mesh
credential, issue `SESSION RESUME` before joining, and then request fresh
credentials. This is the exact-token boundary required for multiple live
clients using the same account and nickname to share one logical session.
Session files are written atomically with owner-only permissions on POSIX.

Inside the desktop client, room tabs are clickable and retain independent
transcripts, rosters, unread counts, and unfinished drafts. The corresponding
keyboard commands are `/join #room`, `/switch #room`, and `/part`. Use
`/view comic`, `/view text`, `/members`, `/avatar name`, and `/dialog IDD_*`
for view and source-dialog workflows. Conversation files and rendered UI
captures use `/open path.ccc`, `/save path.ccc`, and `/export path.png`;
writes are bounded and atomic.

The portable desktop UI has a shared Fluent-style component library and a
separate neutral application font; Comic Neue remains confined to comic
content. See `docs/UI_LIBRARY.md`. Exact headless previews are available with
`zig build run -- render-ui`, plus the `conversation`, `menu`, and `settings`
variants.

`--tls-cert <cert-and-key.pem>` presents a PEM client certificate and private
key for SASL EXTERNAL. Onyx TLS presents the certificate during a verified TLS
1.3 handshake; connections without a client certificate use the same verified
TLS 1.3 transport.

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

## Release packages

The current published release is `comicchat-portable-2026-07-19.2`.
It contains x86_64 binary packages for Windows, Linux, FreeBSD, and OpenBSD,
an explicit buildable source archive, and a single SHA-256 manifest covering
all five artifacts. The source archive includes the pinned Onyx TLS submodule,
so it can be built after extraction without a separate submodule checkout.

Verify downloaded artifacts before use:

```sh
sha256sum -c comicchat-unofficial-modern-builds-2026-07-SHA256SUMS.txt
```

To build the binary archives from a clean checkout:

```sh
./tools/package-release.sh unofficial-modern-builds-2026-07
```

Each archive contains the executable, this README, the AGPL license, and
third-party notices. Comic characters, backdrops, face expressions, and fonts
are embedded in the binary.
`comicchat app <nick>` defaults to the `eshmaki.me` server and `#root` channel;
pass a host and/or channel to override either default.

The direct Wayland client currently uses scale 1. It parses the compositor's
XKB keymap for base and Shift levels and implements compositor-configured
client-side key repeat, with a US evdev fallback before a usable keymap is
available. It does not yet support AltGr/ISO Level3, compose/dead-key sequences,
IME, or output-scale negotiation. Win32 is system-DPI aware rather than
per-monitor-v2 aware. Window creation, configure/resize, shared-memory
presentation, keyboard input, IRC traffic, and clean close are implemented on
both Wayland and Win32. Pointer input and the shared editing clipboard model
are implemented; native OS clipboard and IME bridges remain future work.

The portable lane has no SDL dependency. Native backends speak the Wayland/X11
protocols or Win32 APIs directly, and all display the same software-rendered
comic framebuffer. IRC connections use verified TLS by default on port 6697,
through the pinned Onyx TLS implementation. The client
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

## Design tenets

- **Source-faithful rendering.** Microsoft's original implementation is the
  behavioral source of truth for panel splitting, avatar order and scale,
  emotion selection, text measurement, balloon routing, and tails.
- **One portable core.** Protocol, assets, layout, rendering, and client state
  are platform-independent; native backends own window/event integration and
  framebuffer presentation.
- **Interoperable IRC.** Comic metadata remains compatible with ordinary IRC;
  clients without Comic Chat still see the conversation text.
- **Portable product first.** This repository ships one portable client; it
  does not vendor a second MFC/C++ implementation.

## License and provenance

ComicChat's portable code is licensed under **AGPL-3.0-or-later**. The
historical Comic Chat source is MIT-licensed and remains an external reference
at <https://github.com/microsoft/comic-chat>; its MFC/C++ tree is not vendored
here. Microsoft names, logos, and artwork may be trademarks, and
builds from this repository are unofficial and unsupported. The portable asset
set's historical source and transformation record is kept in
[`legacy/docs/PORTABLE_ASSET_PROVENANCE.md`](legacy/docs/PORTABLE_ASSET_PROVENANCE.md).
The generated portable font atlases are derived from Comic Neue Bold and Bold
Italic under the SIL Open Font License; see
[`src/render/COMIC_NEUE_LICENSE.txt`](src/render/COMIC_NEUE_LICENSE.txt).
The pinned Onyx Server TLS implementation is included as an AGPL-3.0-or-later
submodule under `third_party/onyx-server`.
