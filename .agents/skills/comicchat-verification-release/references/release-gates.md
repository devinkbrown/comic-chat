# Exact-head release gates

## Preflight

~~~sh
git status --porcelain=v1
git rev-parse HEAD
git submodule status --recursive
git diff --check
~~~

Require no status output and no submodule prefix indicating uninitialized, modified, or conflicted state. Confirm third_party/libuv and third_party/mbedtls match .gitmodules and .github/workflows/build-modern.yml.

## Generated and source-derived assets

~~~sh
python3 scripts/build-modern-icons.py lint --complete
python3 scripts/build-modern-icons.py verify
python3 v2.5-beta-1-modern/tests/dialog_chrome_test.py
python3 v2.5-beta-1-modern/tests/modern_icon_pipeline_test.py
python3 v2.5-beta-1-modern/tests/original_artwork_runtime_test.py
python3 v2.5-beta-1-modern/tests/source_strip_topology_test.py
python3 v2.5-beta-1-modern/tests/windows_icon_integration_test.py
~~~

## Portable release build

~~~sh
CC=clang CXX=clang++ meson setup <release-dir> portable --buildtype=release
meson compile -C <release-dir>
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  meson test -C <release-dir> --print-errorlogs
meson test -C <release-dir> --suite perf --verbose
~~~

Run a real Wayland compositor smoke and require a nonempty PNG. Force X11 under a real X server when X11 behavior changed.

## Sanitizers

Require ASan+UBSan for parser, transport, asset decoding, memory, and ownership changes. Require TSan with -Dfrontend=false for worker, queue, callback, stop/restart, or generation changes. Use the Meson commands in AGENTS.md.

## Required CI matrix

Inspect .github/workflows/build-modern.yml at the candidate SHA. At the current baseline it owns:

- Portable strict C++26 with the pinned Linux Clang and real Wayland smoke.
- FreeBSD 15 native build/test/smoke.
- OpenBSD 7.9 native build/test/smoke.
- Windows Visual Studio/MSVC build, pinned dependency verification, package creation, and random-folder smoke.

Inspect .github/workflows/agent-workflow.yml when .agents/, .claude/, .codex/, AGENTS.md, CLAUDE.md, the workflow contract, or scripts/ai/ change.

For every required run, compare its head SHA to git rev-parse HEAD. Do not accept a successful run selected only by branch name.

## Windows packages

The Windows workflow builds pinned x86 libuv and mbedTLS static libraries, builds both modern MFC clients, then runs:

~~~powershell
./scripts/package-modern-builds.ps1
./scripts/smoke-test-modern-builds.ps1
~~~

The expected upload contains:

- ComicChat-1.0-pre-unofficial-modern.zip
- ComicChat-2.5-beta-1-unofficial-modern.zip
- SHA256SUMS.txt

Inspect the current scripts for exact filenames and payload before release; these notes do not override code.

## Primary sources

- GitHub Actions workflow execution and GITHUB_SHA: https://docs.github.com/en/actions/concepts/workflows-and-actions/workflows
- GitHub workflow artifacts: https://docs.github.com/en/actions/concepts/workflows-and-actions/workflow-artifacts
- GitHub release management: https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository
- Secure use of third-party actions: https://docs.github.com/en/actions/security-for-github-actions/security-guides/security-hardening-for-github-actions

Repository workflows and packaging scripts define the release payload. GitHub documentation defines how to bind that evidence to the exact commit and published artifact.
