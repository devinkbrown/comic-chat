# Legacy build and release checklist

## 1. Verify the imported snapshot

From the `legacy` directory:

```sh
./scripts/verify-import.sh
```

For the strongest provenance check, provide a Microsoft upstream checkout
that contains the pinned revision:

```sh
./scripts/verify-import.sh /path/to/microsoft-comic-chat
```

This is the complete source-level gate available on non-Windows hosts. It
checks SHA-256 hashes and the exact imported file inventory. A Linux host
cannot compile this target because Microsoft Foundation Classes are not part
of MinGW, Wine, or the Linux MSVC toolchain.

## 2. Build on Windows

Install the Visual Studio 2022 components listed in `README.md`, then run:

```bat
build.cmd Release --clean
```

The script fails early when Visual Studio, `vcvars32.bat`, NMAKE, the x86
compiler, or the Spectre x86 MFC libraries are missing. It also checks that the
result begins with a DOS/PE signature, is an x86 image, and is not empty.

Modern MIDL regenerates `icchat.h` as part of the upstream build. The wrapper
temporarily preserves and restores the pinned imported header so a successful
build does not dirty the provenance snapshot. Generated `icchat_i.c` and the
`Debug`/`Release` output directories are build products and are excluded from
the inventory portion of `verify-import.sh`.

For diagnostic work:

```bat
build.cmd Debug --clean
```

Do not distribute the Debug CRT build.

## 3. Manual acceptance pass

Run `source\Release\CChat.exe` and verify all of the following on a clean test
account:

1. The main frame, rebar, comic view, member list, and Say box appear.
2. The bundled default character and backdrop render without a download.
3. Resize and maximize the window; panels reflow and remain on-screen.
4. Scroll a multi-panel conversation with the mouse wheel.
5. Connect through a trusted local TLS tunnel or bouncer to a test IRC network.
6. Join a room, send and receive text, change emotion, and switch text/comic
   views.
7. Exit and relaunch; non-sensitive window/view preferences persist.

Never put production credentials into the plaintext IRC transport.

## 4. Package and isolated smoke

```powershell
pwsh -NoProfile -File .\scripts\package.ps1
pwsh -NoProfile -File .\scripts\smoke.ps1 -RunSeconds 10
```

The package and smoke scripts accept `-Configuration Debug` for private
diagnostics, using a visibly different `win32-debug` filename, but Release is
mandatory for distribution. Packaging refuses non-x86 or malformed PE files
and creates `out\SHA256SUMS.txt`.

On a non-Windows host with PowerShell 7, validate the checksum, extracted
layout, runtime inputs, bundled AVB/BGB art, and x86 PE header without trying to
launch the Windows UI:

```sh
pwsh -NoProfile -File ./scripts/smoke.ps1 -ValidateOnly
```

## 5. Release metadata

Any published archive must:

- remain clearly labeled unofficial, unsigned, unsupported, and archival;
- identify upstream revision
  `c7df00f60bc8e9fdef413f139e61f7c37e024684`;
- include `LICENSE.txt`, `NOTICE.txt`, and `PROVENANCE.md`;
- publish the matching line from `SHA256SUMS.txt`; and
- state the Windows x86, plaintext IRC, and legacy Windows Help limitations.

Do not publish an installer, modify user-wide registry state during packaging,
or imply Microsoft sponsorship.
