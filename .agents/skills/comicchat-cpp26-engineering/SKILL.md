---
name: comicchat-cpp26-engineering
description: Implement, refactor, review, and verify Comic Chat's modern C++ surface in strict C++26 mode across the portable Clang/Meson lane and MSVC/MFC lane. Use when changing .cpp/.hpp files, ownership or concurrency, compiler settings, public adapters, dependencies, sanitizers, or build failures under portable/, v1.0-pre-modern/, or v2.5-beta-1-modern/.
---

# Comic Chat C++26 engineering

## Establish the contract

1. Read AGENTS.md and docs/AI-DEVELOPMENT-WORKFLOW.md.
2. Read docs/CPP26-ENGINEERING.md completely before editing C++. Treat a missing file as an integration prerequisite and report it instead of inventing a replacement playbook.
3. Classify the target as portable core, shared transport/protocol, native Windows/MFC, generated glue, third-party C, or immutable history.
4. Inspect the exact source, build declaration, consumers, and tests before proposing a change.
5. Reproduce the defect or encode the new contract with the narrowest causal test.

Read references/toolchain-gates.md before changing language features, toolchain pins, compiler flags, sanitizer configuration, or cross-platform abstractions.

## Preserve repository lanes

- Treat v1.0-pre/, v1.0/, v2.1b/, v2.5-beta-1/, and historical artifacts/ content as read-only source oracles.
- Put portable Linux/BSD implementation in portable/. Keep its headers under portable/include/comicchat/ and implementation under portable/src/.
- Put native MFC adaptations in v1.0-pre-modern/ or v2.5-beta-1-modern/. Keep Win32/MFC behavior native; do not route it through Wine or the SDL frontend.
- Change shared transport and IRCv3 behavior once under portable/, then keep the MFC adapters compiling against that same API.
- Leave vendored third_party/, generated MIDL/C sources, and generated icon outputs alone unless the task explicitly owns their source or generator.

## Design the change

- Make ownership explicit with RAII. Use value types and unique ownership by default; justify shared ownership and every non-owning view by lifetime.
- Keep asynchronous lifetimes joinable, generation-tagged, and cancellation-safe. Do not detach threads or let callbacks outlive their owner.
- Keep C, libuv, Win32, and MFC callbacks exception-safe. Contain exceptions before crossing an ABI callback boundary.
- Represent recoverable boundary failures explicitly. Use std::expected where the surrounding API already uses it; do not mix incompatible error channels casually.
- Preserve hard limits before allocating, parsing, queueing, decompressing, or copying hostile data.
- Zero credentials and authentication intermediates on every terminal and exceptional path. Do not rely on ordinary std::string destruction for secrets.
- Use span and string_view only while the referent's lifetime is provable. Copy data that must survive a callback, poll, receive-buffer reuse, or generation change.
- Prefer standard C++ facilities only after verifying support in both required lanes. A C++26 paper or Clang implementation does not prove current libc++ and MSVC library support.
- Keep public adapters narrow and typed. Do not leak SDL, Cairo, libuv, mbedTLS, MFC, HWND, or platform-only types into unrelated core layers.
- Avoid opportunistic rewrites. Make the smallest coherent change that preserves source fidelity and platform parity.

## Prove the result

Run the causal test first, then the affected ladder. Use fresh build directories when compiler or Meson options change.

~~~sh
git diff --check
CC=clang CXX=clang++ meson setup <build-dir> portable --buildtype=release
meson compile -C <build-dir>
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  meson test -C <build-dir> --print-errorlogs
~~~

For parser, ownership, transport, memory, or concurrency changes, run the applicable sanitizer lanes:

~~~sh
CC=clang CXX=clang++ meson setup <asan-dir> portable \
  -Db_sanitize=address,undefined -Db_lundef=false
meson test -C <asan-dir> --print-errorlogs

CC=clang CXX=clang++ meson setup <tsan-dir> portable \
  -Db_sanitize=thread -Db_lundef=false -Dfrontend=false
meson test -C <tsan-dir> --print-errorlogs
~~~

- Compile the MSVC consumer when shared headers or sources change. Use the exact Visual Studio environment and nmake command from v2.5-beta-1-modern/chat.mak or rely on the native Windows CI job; never claim MSVC coverage from a Clang syntax check.
- Run generated icon verification when resource declarations, app identity, packaging, or native UI surfaces change.
- Treat a skipped platform, timed-out test, sanitizer report, warning under werror, or unavailable command as missing evidence rather than a pass.
- Report compiler versions, commands, exit codes, pass counts, source/spec oracle, changed paths, and residual platform risk.
