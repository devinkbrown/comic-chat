# Comic Chat: Reinked agent guide

This repository contains Microsoft's historical Comic Chat sources and the
unofficial **Comic Chat: Reinked** modernization. Read
[`docs/AI-DEVELOPMENT-WORKFLOW.md`](docs/AI-DEVELOPMENT-WORKFLOW.md) and the
canonical [`docs/CPP26-ENGINEERING.md`](docs/CPP26-ENGINEERING.md) playbook
before investigating, implementing, reviewing, or integrating non-trivial C++
work. Protocol and connection-path changes must also use the current
[`docs/IRCv3-COVERAGE.md`](docs/IRCv3-COVERAGE.md) and
[`docs/TRANSPORT-RETIREMENT.md`](docs/TRANSPORT-RETIREMENT.md) ledgers.

## Source of truth

- The original Microsoft source snapshots (`v1.0-pre`, `v1.0`, `v2.1b`,
  `v2.5-beta-1`) live in `version/*` archival branches, not on `main`; treat
  them and the historical contents of `artifacts/` as reference snapshots. The
  source-fidelity reference bitmaps and font used by the portable build were
  relocated onto `main` under `portable/assets/`. Do not modernize any snapshot
  in place.
- Derive rendering, layout, character, and interaction behavior from the
  original Microsoft source and artwork before changing a modern renderer.
  Record the source file or asset that establishes the behavior.
- Put the native Unix/BSD implementation in `portable/`; put the modern native
  Windows/MFC implementation in the matching `*-modern/` tree. Shared transport
  and IRCv3 policy live in `portable/` and are consumed by both frontends.
- Prefer official protocol specifications and primary library documentation.
  A model's recollection is not a protocol oracle.

## Engineering constraints

- Portable C++ must compile in strict C++26 mode with Clang 21 or newer. The
  Windows/MFC build uses current MSVC in `/std:c++latest` mode; keep it valid
  there and use Clang cross-syntax checks where they provide real coverage.
  Keep Windows code native Win32/MFC and Unix/BSD code native
  SDL3/Cairo/Wayland/X11.
- Keep the libuv + mbedTLS transport bounded, asynchronous, generation-safe,
  and free of implicit plaintext fallbacks. Zero credentials and authentication
  intermediates on every success, error, cancellation, and exception path.
- Preserve legacy wire compatibility while adapting negotiated IRCv3 shapes
  before exposing capabilities to the old UI/model.
- Do not claim visual fidelity from a placeholder. Compare against the original
  source path and generated visual evidence.
- Do not edit generated icon binaries manually. Change their declared sources
  or generator, then rebuild and verify the complete catalog.

## Collaboration rules

- Codex owns scope, integration, final review, commits on the integration
  branch, remote publication, and release decisions.
- Claude is an independent researcher, implementation drafter, or adversarial
  reviewer. Claude never merges, pushes, publishes, or supplies the only review
  of its own patch. Claude does not receive Bash; it proposes verification
  commands and Codex executes them.
- Use one writer per linked worktree. Parallel agents may read the same tree,
  but parallel writers must have separate worktrees and non-overlapping scopes.
- Every handoff uses the schema in the workflow document. Integrators inspect
  the actual diff and rerun evidence; never accept a handoff on prose alone.
- Keep the main thread for requirements and decisions. Delegate noisy scans,
  logs, and independent reviews, and return distilled findings with file/line
  evidence.

## Verification

Start with the narrowest causal regression, then run all affected gates. Before
integration, at minimum:

```sh
git diff --check
python3 scripts/build-modern-icons.py lint --complete
python3 scripts/build-modern-icons.py verify
CC=clang CXX=clang++ meson setup <build-dir> portable --buildtype=release
meson compile -C <build-dir>
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  meson test -C <build-dir> --print-errorlogs
```

For transport, parser, concurrency, ownership, or secret-handling changes, also
run the relevant ASan+UBSan suite and a TSan suite where supported:

```sh
CC=clang CXX=clang++ meson setup <asan-dir> portable \
  -Db_sanitize=address,undefined -Db_lundef=false
meson test -C <asan-dir> --print-errorlogs

CC=clang CXX=clang++ meson setup <tsan-dir> portable \
  -Db_sanitize=thread -Db_lundef=false -Dfrontend=false
meson test -C <tsan-dir> --print-errorlogs
```

The relevant Linux, Wayland, FreeBSD, OpenBSD, and native Windows CI jobs must
all pass on the integrated commit before release. A timeout, skipped platform,
or sanitizer finding is not a pass.
