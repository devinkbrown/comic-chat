# ComicChat legacy port

This is the faithful legacy lane of the ComicChat project: Microsoft Chat 2.5
beta 1, kept as a Win32 MFC application and made reproducible on a current
Windows/Visual Studio toolchain. The repository's Zig implementation remains
the portable, newly engineered client; this directory preserves and advances
the original application rather than replacing it with another UI rewrite.

The imported source is pinned to Microsoft's upstream revision
`c7df00f60bc8e9fdef413f139e61f7c37e024684`. See `PROVENANCE.md` and run the
integrity check before changing imported files:

```sh
./scripts/verify-import.sh
```

## Supported build

The supported target is **Windows x86**. It requires:

- Windows 10 or 11;
- Visual Studio 2022 with Desktop development with C++;
- the C++ MFC component for the current x86/x64 toolset;
- the MSVC v143 x86 build tools and a Windows SDK; and
- the Spectre-mitigated x86 MFC libraries used by the upstream makefile.

From a normal Command Prompt (a Developer Prompt is not required):

```bat
cd legacy
build.cmd Release --clean
```

The script locates Visual Studio through `vswhere.exe`, initializes the x86
compiler environment, checks the required MFC library, removes stale objects
when `--clean` is present, and runs the pinned NMAKE build. The result is
`source\Release\CChat.exe`. Use `build.cmd Debug --clean` for MFC assertions
and DebugView traces.

The makefile does not track header dependencies. Always use `--clean` after
changing a header or switching toolsets.

## Package and smoke test

After a Release build, create an archival ZIP and SHA-256 file:

```powershell
pwsh -NoProfile -File .\scripts\package.ps1
```

Then exercise the ZIP from a random path containing spaces and an unrelated
working directory:

```powershell
pwsh -NoProfile -File .\scripts\smoke.ps1
```

The smoke test verifies the package checksum, x86 PE header, required runtime
files, bundled avatar/backdrop art, successful process startup, creation of a
top-level window, and that the client remains running for the test interval.
It terminates the test process afterward. Output is written to `out/` and is
intentionally not a source input.

On Linux or macOS, the same script can validate every package property except
launching the Win32 UI:

```sh
pwsh -NoProfile -File ./scripts/smoke.ps1 -ValidateOnly
```

PowerShell 5.1 also works: replace `pwsh` with `powershell`.

## Runtime scope

This lane intentionally preserves the original MFC/OLE/Win32 architecture and
the original AVB/BGB behavior. Microsoft's upstream modern tree already adds a
current NMAKE build, static MFC linkage, Common Controls v6, modern IRC JOIN and
RichEdit compatibility, bundled-art lookup, on-screen window placement, safe
ctype use, mouse-wheel scrolling, and uniform Windows DPI virtualization.

Known constraints remain:

- The executable is x86. An x64 conversion is a separate ABI and pointer-width
  audit, not a supported configuration.
- IRC transport is plaintext. Do not send account passwords over an untrusted
  network; use a trusted local TLS tunnel/bouncer until authenticated SChannel
  transport is ported and verified.
- The old `.hlp` format is not supported by a default modern Windows install.
- The build is unsigned and has no installer or auto-updater.
- NetMeeting integration and the original online art servers are disabled.
- The UI is DPI-virtualized rather than per-monitor-v2 aware.
- The 1998 IRC, URL, scripting, and DCC/file-transfer surfaces have not had a
  modern hostile-input security audit. Use a test account and do not accept
  unsolicited files.

See `source/README.md` for Microsoft's detailed modernization notes and
`BUILDING.md` for the validation and release checklist.
