# Comic Chat: Reinked C++26 engineering playbook

This is the canonical operating contract for agents and humans changing C++ in
Comic Chat: Reinked. Read it before investigating, implementing, reviewing, or
validating a C++ change. It supplements `AGENTS.md` and
`docs/AI-DEVELOPMENT-WORKFLOW.md`; the stricter rule wins.

Exceptional work in this repository is not “modern-looking C++.” It is a small,
causal change that preserves Microsoft Comic Chat behavior, proves every
affected consumer and build lane, has explicit lifetime and failure semantics,
and leaves evidence another engineer can reproduce. Compiler, test, sanitizer,
protocol, source-art, runtime, and benchmark evidence outrank model agreement.

## Agent operating modes

Assign one mode and a bounded question per agent. Do not ask a single worker to
discover, redesign, implement, bless its own patch, and release it.

- A **change-graph investigator** is read-only. It returns symbols, consumers,
  build/test targets, original/spec oracles, ownership/thread boundaries, and
  unresolved questions with file/line evidence.
- A **C++ implementer** owns one isolated worktree and one non-overlapping file
  scope. It establishes the causal test, makes the smallest coherent patch, and
  returns exact verification evidence; it does not integrate or publish.
- A **build resolver** reproduces one compiler/link/resource failure and fixes
  its root cause surgically. It must not use a compile error as permission for
  a broad refactor or behavioral workaround.
- A **security/protocol reviewer** is read-only and specification-led. It traces
  hostile input, bounds, downgrade/failure policy, sensitive copies, wipes, and
  legacy adaptation before considering style.
- A **lifetime/concurrency reviewer** is read-only and enumerates owners, last
  callbacks, cancellation outcomes, lock order, re-entry, stop/restart, stale
  generations, and destruction interleavings.
- A **render/UI fidelity reviewer** compares the modern result with named
  Microsoft sources/assets and native platform behavior. It does not approve a
  placeholder because it looks plausible.
- A **performance reviewer** demands a causal hypothesis and comparable raw
  measurements; it rejects unmeasured “optimization.”
- A **verifier** runs commands on the frozen patch and reports artifacts and
  exact exit results without silently fixing failures. Any subsequent edit
  invalidates that evidence.
- The **integrator** inspects the actual diff, reconciles independent findings,
  reruns affected gates on the merged head, and alone decides publication.

Use a fresh specialist for high-risk review. Author and reviewer may share an
oracle, never an unexamined conclusion.

## 1. Prove that you are in the right repository

The local repositories have confusing names. The Microsoft legacy fork is
`/home/kain/comic-chat-legacy-fork`. `/home/kain/comicchat` is a different
project and is never a C++ working directory for this playbook. Linked
worktrees for the legacy fork may live elsewhere, so verify the Git common
directory and repository sentinels rather than trusting `pwd` or the directory
basename.

Run this before the first search or edit:

```sh
root="$(git rev-parse --show-toplevel)" || exit 1
common="$(realpath "$(git rev-parse --git-common-dir)")" || exit 1
printf 'root=%s\ncommon=%s\nhead=%s\n' \
  "$root" "$common" "$(git rev-parse HEAD)"
test "$root" != /home/kain/comicchat
if test -d /home/kain/comic-chat-legacy-fork/.git; then
  test "$common" = /home/kain/comic-chat-legacy-fork/.git
fi
test -f "$root/portable/meson.build"
test -f "$root/v2.5-beta-1-modern/chat.mak"
test -f "$root/v2.5-beta-1/panel.cpp"
git status --short
```

Stop if any sentinel fails. Record the full base commit and initial porcelain
status. A dirty primary worktree is not permission to overwrite someone else's
work. Parallel writers use separate linked worktrees based on the assigned
commit; each worktree has exactly one writer.

## 2. Know the source topology and the authority of each tree

The tree is deliberately split by purpose:

| Path | Role | Editing rule |
|---|---|---|
| `v1.0-pre/`, `v1.0/`, `v2.1b/`, `v2.5-beta-1/` | Released Microsoft source and artwork snapshots | Read-only behavioral oracle |
| historical contents of `artifacts/` | Archived headers, libraries, and build evidence | Read-only reference |
| `v1.0-pre-modern/`, `v2.5-beta-1-modern/` | Native Win32/MFC modernization | Edit for Windows work |
| `portable/` | Shared C++ core and native SDL3/Cairo Unix/BSD frontend | Edit for portable and shared work |
| `portable/assets/icons/` | Declared modern icon sources and generated catalog | Edit sources/manifest, never generated binaries directly |
| `third_party/` and `portable/subprojects/` | Pinned dependency sources and fallback build descriptions | Change only as a separately reviewed dependency update |

Never “clean up” a Microsoft snapshot. Read the old implementation, cite the
specific source/resource that establishes behavior, then implement the modern
equivalent in a `*-modern/` tree or `portable/`. Keep the awkward original
artifact if it is the only fidelity oracle. If a modern tree must copy a rule,
record the original path and symbol in a comment, test, or review evidence.

Do not infer behavior from filenames or screenshots alone. For rendering and UI
work, triangulate the implementation (`panel.cpp`, `pageview.cpp`,
`avatar.cpp`, `bodycam.cpp`, `balloon.cpp`, resource scripts), the original
artwork/metrics, and a deterministic output or the archived executable. A
placeholder that merely resembles Comic Chat is not a port.

## 3. Respect the real build and consumer lanes

There is no single generic C++ build.

### Portable and shared Clang lane

`portable/meson.build` is the build authority. It requires Meson 1.4 or newer,
Clang 21 or newer, `cpp_std=c++26`, `-pedantic-errors`, warning level 3, and
warnings as errors. Meson currently maps this mode to Clang's `-std=c++2c`.
`portable/include/comicchat/cpp26.hpp` rejects C++23-or-older mode. The shared
core includes layout, rendering, assets, text, memory, scheduling, libuv
transport, DCC, and IRCv3.

The portable frontend is native SDL3/Cairo, not Wine and not a Windows UI
emulation. SDL selects Wayland or X11 on Linux. The same Meson source also
builds on FreeBSD and OpenBSD; Linux-only APIs require an explicit guarded
adapter and native BSD proof.

### Native Windows lane

The modern Windows clients remain native x86 Win32/MFC and NMAKE projects. The
primary `v2.5-beta-1-modern/chat.mak` uses current MSVC with
`/std:c++latest`, `/permissive-`, `/Zc:__cplusplus`, the conforming
preprocessor, `/W4`, and a force-included `cpp26mode.h`. It intentionally
compiles generated MIDL glue and third-party/legacy C as C rather than
mislabeling it as C++.

`v1.0-pre-modern/chat.mak` is a separate legacy-compatible MFC/NMAKE lane and
is also built by Windows CI. Its generated Visual C++ 4.2-era flags are not a
C++26 proof and must not be confused with the v2.5 C++26 migration. Check it
when a change touches shared packaging, resources, dependencies, or release
behavior; modernize it only in an explicitly assigned scope, not incidentally.

`/std:c++latest` is a working-draft mode, not a promise that every C++26 feature
exists; Microsoft says its contents can change. Use it because it is this
repository's Windows lane, then probe each new feature separately. See the
[MSVC `/std` documentation][msvc-std] and [NMAKE reference][nmake].

### Shared-source consequence

These files under `portable/` are also direct objects in the Windows makefile:

- `src/crypto_runtime.cpp`
- `src/memory.cpp`
- `src/net/connection_engine.cpp`
- `src/net/dcc_transfer_engine.cpp`
- `src/net/ircv3.cpp`
- `src/sound.cpp`

Their public headers are consumed by the MFC adapters. A Clang-only success is
therefore incomplete. Search `chat.mak`, `ircsock.*`, `filesend.*`,
`ircv3eventbridge.h`, and `tests/transport_adapter_api_compile.cpp` before
changing a shared signature, type property, callback contract, error enum, or
layout. Clang cross-syntax checks are useful early evidence; only native MSVC,
resource compilation, MFC linking, launch, and smoke tests prove Windows.

## 4. Deep-search before proposing a design

An exceptional C++ agent produces a change graph before it produces code. For
every changed symbol or behavior, find all of these that exist:

1. public declaration and semantic comments;
2. every definition, overload, constructor, destructor, move operation, and
   callback trampoline;
3. every direct and indirect caller/consumer;
4. ownership transfer, thread handoff, queue, and generation boundary;
5. parser/serializer or resource contract;
6. Meson target, NMAKE object/dependency, and CI job;
7. focused tests, fixtures, fault injection, stress tests, and benchmarks;
8. original Microsoft implementation/resource and official protocol/library
   specification;
9. platform-specific branch and fallback implementation;
10. documentation or release claim that the change could make inaccurate.

Use repository-aware searches, not one symbol lookup:

```sh
rg -n --hidden --glob '!third_party/**' --glob '!portable/subprojects/packagecache/**' \
  'Symbol|wire-token|resource-id' portable v1.0-pre-modern v2.5-beta-1-modern
rg -n 'source\.cpp|public_header|test\(' portable/meson.build
rg -n 'source\.obj|public_header|RESOURCE_INPUTS' \
  v1.0-pre-modern/chat.mak v2.5-beta-1-modern/chat.mak
rg -n 'original-symbol|resource-id|numeric|layout-constant' \
  v1.0-pre v1.0 v2.1b v2.5-beta-1
rg -n 'feature-or-file' .github/workflows docs README.md
git log --oneline --all -- path/to/file
git blame -L start,end -- path/to/file
```

Read complete functions and adjacent state, not isolated matching lines. Trace
both directions: “who calls this?” and “what does this call or retain?” For C
callbacks, also trace `data` pointers and the storage containing the C handle or
request. For UI resources, trace numeric IDs through `.rc`, `resource.h`,
catalog tables, image-list indices, command handlers, and the generator.

Before editing, write a compact evidence packet:

```text
goal and observable done condition
base commit and clean/dirty status
symbols and files in the change graph
Microsoft source or official spec oracle
affected build/platform consumers
ownership and thread model
failure, cancellation, and backpressure policy
causal test that should fail first
required post-change gates
```

If any line is unknown, continue discovery or label the uncertainty. Do not fill
gaps with a likely-sounding architecture.

## 5. Use causal TDD, not ceremonial test addition

For a defect, first create the smallest regression that demonstrates the
reported failure on the assigned baseline. Capture the command, exit code, and
exact failing assertion or sanitizer report. Then make the implementation pass
that same test. A test that was already green is not a red test.

For a feature, first encode an executable contract at the narrowest real
boundary. Examples:

- parser input plus exact accepted/rejected state and bounded storage;
- transport command through callback/event delivery, cancellation, and restart;
- MFC adapter compile contract plus legacy-model event shape;
- original asset through decoder/compositor with source-derived dimensions or
  topology;
- deterministic render input through a PNG or structural golden;
- build-resource dependency proving a changed source rebuilds the binary.

Test the failure path before the happy path when the risk is allocation,
truncation, timeout, malformed wire data, full queue, partial write, callback
re-entry, cancellation, or crypto failure. Fault injection belongs at an
existing seam, not behind a production behavior toggle.

Never weaken an assertion, increase a timeout, disable a sanitizer, or broaden
an ignore list merely to make the patch green. First prove why the prior oracle
was wrong. Do not hard-code changing aggregate test counts in documentation;
record the exact test names and command results in the handoff generated for
that commit.

## 6. Modern C++ ownership: use the type that matches the real lifetime

Follow the [C++ Core Guidelines' resource rules][core-guidelines], but apply
them to this codebase rather than as generic style dogma.

### Preferred representation

- Use values for small immutable state and return-by-value when identity is not
  required.
- Use `std::unique_ptr` for exclusive heap identity and PIMPL boundaries.
- Use `std::shared_ptr<const T>` only where immutable data truly crosses
  independently completing consumers, as with queued receive/render snapshots.
- Use `std::span` or `std::string_view` for a non-owning, bounded call-duration
  view. Never retain it beyond its owner's proven lifetime.
- Use `std::expected<T, E>` for expected operational failure that callers must
  handle. Do not collapse protocol, queue, allocation, and lifecycle failures
  into `bool` when recovery differs.
- Use a named RAII wrapper for a resource with one clear owner and cleanup
  operation. Make it non-copyable and explicitly movable only if transfer is
  valid.

Raw pointers and references are acceptable non-owning observers where the MFC
framework, a parent object, or a C callback contract owns the referent. State
that relationship. Replacing a well-defined observer with `shared_ptr` can hide
a cycle, prolong a window past destruction, or cross an invalid thread; it is
not automatic modernization.

### Resource-specific rules

- A successful `uv_*_init` creates a handle that must eventually receive
  `uv_close`; its storage cannot be reclaimed or reused until the close callback
  runs. A `unique_ptr<uv_tcp_t>` with an ordinary deleter is therefore wrong.
- A libuv request is not a handle. It completes or is cancelled under its own
  request callback contract; only supported request types can be passed to
  `uv_cancel`.
- mbedTLS contexts must be initialized and freed in the documented order. A
  partially initialized aggregate still needs cleanup for every initialized
  member.
- Windows `HWND`, MFC `CWnd*`, GDI objects, image lists, and resource handles
  have framework/thread/selection rules. Prefer the existing MFC wrapper when
  it expresses them; never delete a framework-owned `CWnd*`.
- Callback state must outlive the last possible callback. Clearing the callback
  pointer is not enough if work is already queued.

Destructors must be bounded, non-throwing, and safe for every constructible
state. Do not detach a thread to avoid a difficult join. Do not invoke user or
UI callbacks while holding an internal mutex. C callback boundaries catch all
exceptions and translate them into a terminal diagnostic or controlled error;
no exception may unwind through C, libuv, mbedTLS, Win32, or MFC.

## 7. C++26 is a verified capability, not a decoration

The current C++26 working draft is [WG21 N5046][cpp26-draft]. Compiler support
is intentionally incremental; Clang publishes a [C++2c implementation
matrix][clang-cxx-status], and WG21 SD-6 defines [feature-test
recommendations][sd6]. Therefore:

1. Keep the repository-wide language-mode gates in `cpp26.hpp` and
   `cpp26mode.h`.
2. For every newly used library or language facility, check its `__cpp_*` or
   `__cpp_lib_*` macro at the required revision, usually after including
   `<version>`.
3. Compile the exact translation unit with Clang 21 and the current Linux lane,
   then with native MSVC `/std:c++latest` if Windows consumes it.
4. Add a localized compatibility wrapper only when a required platform library
   lacks the facility. Test both native and forced-fallback branches.
5. Do not key a standard-library feature solely from compiler version,
   `__cplusplus`, `_MSC_VER`, or the marketing name “C++26.”

The code already uses `std::expected`, `std::span`, ranges, PMR, and a
feature-probed `std::jthread`/`std::stop_token` compatibility layer. Extend
those patterns instead of inventing parallel result, buffer, allocator, or
thread abstractions. A new C++26 feature must simplify a real invariant or
produce measured value. Avoid draft novelty in a shared public interface when a
stable facility is equally clear, especially where MSVC and BSD libc++ differ.

Do not use modules, coroutines, executors, `std::simd`, or reflection merely to
claim a newer standard. First prove implementation availability, build-system
support, debugger/tooling behavior, cancellation semantics, and both consumers.

## 8. Treat every parser and size as a security boundary

Untrusted inputs include IRC lines and tags, CAP/ISUPPORT values, BATCH and
multiline state, SASL challenges, CTCP/DCC fields, CTCP SOUND names, proxy
replies, TLS records/certificates, configuration strings, downloaded files,
and legacy ICO/BMP/DIB/AVB/zlib assets.

For each parser:

- establish a byte limit before buffering or allocating;
- distinguish wire bytes from decoded characters, code points, fields, lines,
  and logical payload bytes;
- check `offset <= size` before subtraction and `amount <= limit - used` before
  addition; check multiplication before computing dimensions or allocation;
- use unsigned fixed-width types only where the wire format requires them, then
  convert once with a checked bound;
- preserve the protocol's ASCII case rules; do not use locale-sensitive
  classification on raw bytes;
- remember that `std::string` and `std::string_view` can contain embedded NUL;
  reject it before any C, filesystem, Win32, MFC, libuv, or mbedTLS API that
  interprets a terminator;
- reject or ignore invalid input exactly as the governing specification says;
- reset partial state on terminal error, disconnect, timeout, batch abort, and
  generation change;
- never truncate authentication material, tags, paths, filenames, or resource
  structures into a different valid meaning;
- return typed failure and ensure diagnostics contain no secrets or attacker-
  controlled terminal escapes.

CTCP SOUND and transferred filenames are names, not trusted paths. Reject
absolute paths, separators, dot segments, embedded NUL, Windows device names,
and disallowed extensions before resolution; resolve beneath an allowlisted
root and verify containment. DCC sizes and offsets remain checked 64-bit values,
and receive credit advances only after the adapter commits the file write.

IRCv3 tag and line limits are not interchangeable. Read the current
[message-tags specification][ircv3-tags], [multiline specification][ircv3-multiline],
and capability-specific document before coding. Count precisely what that
specification counts. A parser-only success does not prove the legacy adapter
can consume the negotiated shape, so a capability is auto-requested only after
the adapter/model path is complete and tested.

Every queue is bounded in both item count and retained bytes. Reserve control
headroom so `Disconnect`, close/error events, PONG, authentication, and
cancellation can progress under chat or bulk saturation. When capacity is
exhausted, reject, defer, pause reads, or close according to the explicit
policy—never grow without limit. Backpressure must propagate to the producer;
dropping a completion event while retaining its buffer is not backpressure.

For high-risk pure parsers, add a libFuzzer target or a replayable corpus when
it materially expands malformed-input coverage. LLVM's [libFuzzer
documentation][libfuzzer] explains the in-process model. A fuzzer supplements
deterministic boundary tests; it does not replace them.

## 9. libuv ownership, close, cancellation, and backpressure

The [libuv design contract][libuv-design] is mandatory: loops and handles are
not thread-safe unless documented otherwise, initialized handles must be
closed, close callbacks delimit storage reuse, and requests have separate
completion lifetimes. `uv_run` is not re-entrant; see the [loop API][libuv-loop].

The repository model is one cooperatively stopped owner thread per engine. That
thread owns the loop, socket, poll/timer/async handles, TLS context, request
objects, and protocol pipeline. Other threads submit immutable or move-owned,
generation-tagged commands and wake the loop through `uv_async_t`. They do not
call arbitrary libuv handle APIs directly.

For every asynchronous operation, document this state tuple:

```text
owner thread
allocation owner
operation-start point
buffer lifetime
completion callback
cancellation capability and result
close initiation
last callback
storage reclamation
generation that may observe the result
```

Required lifecycle rules:

- If initialization succeeds, close exactly once even when later setup fails.
- Keep handle storage alive through `uv_close_cb`; keep request and write-buffer
  storage alive through its completion callback.
- `uv_cancel` is best-effort and applies only to documented request types. A
  worker already running needs cooperative cancellation, and its after-callback
  still runs with the documented status. See libuv's [thread/cancellation
  guide][libuv-cancel].
- Stop new work first, invalidate the generation, request/cancel pending work,
  close active handles on the loop thread, drain close/completion callbacks,
  verify `uv_loop_close` does not return `UV_EBUSY`, then reclaim the loop.
- Pause reads when receive-event byte credit is exhausted. Resume only after
  the consumer releases sufficient credit; do not poll on a short timer.
- Partial TLS/plain writes retain the unsent tail and its sensitivity bit.
  Completion means the transport accepted all bytes or emitted a terminal
  failure, not that one syscall succeeded.
- A failed wakeup is a lifecycle error, not permission to mutate the loop from
  the posting thread.

Test start, stop-before-connect, stop during DNS/proxy/TLS, remote close,
timeout, full queues, partial reads/writes, callback-triggered stop, repeated
start/stop, moved-from wrappers, and destruction. Treat `UV_EBUSY`, a live
handle after stop, a callback into a dead generation, or a timer spin under
backpressure as release-blocking.

## 10. mbedTLS, TLS, SASL, and secret handling

The repository compiles its pinned mbedTLS sources into both native lanes so a
system package cannot silently change the crypto ABI or policy. Dependency
updates are security changes: verify the exact commit, upstream release notes,
and current [Mbed TLS security advisories][mbedtls-advisories], then rerun
transport, crypto, Windows, and sanitizer gates.

### TLS invariants

- Initialize the process crypto runtime once and fail closed if entropy/PSA
  initialization fails.
- Default to TLS 1.2 or newer. Set SNI/hostname and require certificate-chain
  plus hostname verification against the selected trust store.
- Never fall back from failed TLS to plaintext. Plaintext is a separate,
  explicit caller policy and must not carry mechanisms requiring a secure
  transport.
- Drive mbedTLS non-blockingly through the libuv readiness state. Treat
  `MBEDTLS_ERR_SSL_WANT_READ` and `MBEDTLS_ERR_SSL_WANT_WRITE` as requested
  readiness, not failure or busy-loop permission. The Mbed TLS porting guide
  documents [custom nonblocking BIO callbacks][mbedtls-porting].
- Reset or reconstruct every per-connection field on reconnect, proxy retry,
  session resume, and generation change. Never assume a reset API clears fields
  outside its documented contract.
- Keep certificate diagnostics useful but bounded; do not include credentials,
  private keys, raw challenges, session secrets, or sensitive outgoing bytes.

### SASL and SCRAM invariants

The protocol authorities are [RFC 4422][rfc4422], [RFC 5802][rfc5802],
[RFC 7677][rfc7677], applicable channel-binding RFCs, and the current IRCv3
[SASL specification][ircv3-sasl]. Implement the exact state machine rather than
accepting numerics by resemblance.

- Select mechanisms only from the server offer and local secure policy.
- Bind SCRAM proofs to the exact GS2 header/authzid and transcript bytes.
- Generate production nonces from the shared OS-seeded PSA CSPRNG. Deterministic
  nonces exist only at an explicit test seam.
- Bound decoded challenge length, salt length, iteration count, and cumulative
  work before PBKDF/HMAC allocation or CPU work.
- Verify the server-final signature before acknowledging success; compare
  authentication values in constant time.
- Distinguish IRCX/server-profile numerics from SASL numerics by current
  authentication state and negotiated mechanism.
- Abort and wipe on malformed base64, unexpected challenge order, timeout,
  cancellation, disconnect, numeric failure, exception, or allocation failure.
- STS policy is keyed to the canonical hostname and must persist/enforce expiry
  and port policy before a future plaintext connection. An in-memory CAP value
  alone is not STS enforcement; consult the [IRCv3 STS specification][ircv3-sts].

### Secrets

Use `LockedSecret` for retained credentials. Memory locking reduces swapping;
it does not replace lifetime minimization or wiping. Avoid extra `std::string`
copies, including hidden short-string-optimization copies. Prefer consuming
rvalue inputs when the API promises consumption, then actively overwrite the
source object as well as the destination/intermediates.

Wipe password, proxy credential, nonce-derived secrets, salted password,
client/server keys, signatures, proofs, decoded AUTHENTICATE chunks, sensitive
queued sends, and scratch buffers on every exit path. Use
`mbedtls_platform_zeroize`, whose [API contract][mbedtls-zeroize] is designed
to resist dead-store elimination. Do not substitute ordinary `memset`. Ensure
moves leave the source empty/wiped and destructors are safe after partial
construction or failed locking. Tests should search the relevant object/queue
storage after success, failure, rejection, move, cancellation, and stop.

## 11. Concurrency, restart, re-entrancy, and multicore work

Before modifying concurrent code, write an ownership table for each mutable
field: owning thread, mutex/atomic protection, callback access, generation,
and destruction point. If ownership is unclear, the design is not ready.

Rules:

- Prefer single-owner state plus message passing over shared mutable state.
- Use `threading::JThread` and stop tokens. Its BSD fallback is part of the
  supported product and must remain join-owning; no detached escape hatch.
- A public `stop()` reached from the worker's own callback is request-only. An
  external caller performs the join. Never self-join and never destroy the
  engine from its active callback.
- Do not hold engine, queue, or UI locks while invoking user callbacks, posting
  window messages, joining, waiting on a future, or calling a potentially
  re-entrant library function.
- Use generation IDs to make stale commands/events harmless. Increment and
  publish the generation under the same synchronization that resets queues and
  visible state.
- Give each atomic an explicit invariant and memory-order argument. `relaxed`
  is for independent telemetry or when another synchronization edge carries
  the data; it is not a performance incantation.
- Establish one lock order and never call into an unknown owner while holding a
  lower-level lock.
- Treat notifier replacement, callback-triggered polling/posting/stop, rapid
  restart, and move construction/assignment as ordinary states, not edge cases.

The [C++ Core Guidelines concurrency section][core-concurrency] recommends
minimizing writable sharing and validating with tools. Clang's [Thread Safety
Analysis][clang-thread-safety] can strengthen stable mutex contracts, but its
annotations do not replace TSan or stress tests.

“Use more cores” is not a design. Keep network I/O on its libuv loop thread and
UI/render presentation on the native UI thread. Send bounded, immutable work to
`WorkerScheduler` only for CPU work large enough to amortize queueing and cache
cost. Cancellation and generation invalidation must cross that boundary. Start
with deterministic one-worker tests, then stress the production schedule.

## 12. UI, rendering, and Microsoft-source fidelity

The old source is the behavior specification; the modern UI may improve
resolution, accessibility, DPI response, and native chrome without changing
Comic Chat's semantic composition.

For every visual change, record:

1. original implementation symbols and resource files;
2. logical coordinates, ordering, state/index mapping, palette/alpha rule, and
   text metric established by those sources;
3. modern transform and which details are intentionally changed;
4. deterministic render or runtime comparison at representative DPI/scales;
5. Windows and Unix/Wayland behavior where platform chrome differs.

Preserve the original 1,440-unit logical coordinate model and transform at the
display boundary. Do not repeatedly round layout during scaling. Shape Unicode
through FreeType/HarfBuzz/ICU; do not replace source metrics with arbitrary
widget defaults. Keep avatar component anatomy, pose selection, mirroring,
occlusion, balloon attachment, panel order, and toolbar command indices tied to
their source rules. High-DPI smoothing may modernize ink edges; it may not move
eyes, limbs, silhouette, expression, or hit targets without an explicit design
decision and visual approval.

UI objects stay on their native UI thread. Network callbacks publish bounded
immutable events; the MFC message pump or SDL loop consumes them. Never update
`CWnd`, GDI, Cairo/SDL presentation state, or Wayland-facing window state from
the network worker. SDL documents its [main-thread model][sdl-main-thread].

Use native window behavior. Windows dialogs use appropriate MFC/Win32 styles,
system close/minimize affordances, DPI APIs, keyboard navigation, and theme
metrics. SDL3 favors Wayland on Linux; missing or odd decorations can depend on
libdecor and compositor support, as documented by SDL's [Wayland
notes][sdl-wayland]. Test a real Wayland compositor, not only the dummy driver.

Headless output is necessary but not sufficient. Structural tests prove source
constants/topology; deterministic PNGs make visual review reproducible; native
runtime evidence proves window chrome, DPI, focus, input, and compositor
behavior.

## 13. Asset and icon pipeline

Never edit generated ICO/PNG/BMP/catalog outputs by hand. The editable contract
is `portable/assets/icons/manifest.json`, SVG masters, and declared optical-size
overrides. `scripts/build-modern-icons.py` owns deterministic generation,
resource includes, hashes, topology, and the lock catalog.

An asset change must:

- identify the matching Microsoft icon, bitmap strip, expression, character,
  command, and resource ID;
- preserve semantic catalog coverage and source index order;
- author a detailed vector/master, including optical variants where simple
  scaling loses legibility;
- regenerate the complete catalog atomically;
- prove Windows `.rc`/NMAKE dependencies and portable installed assets both
  consume it;
- keep the audited source-art fallback intact;
- run lint, pipeline tests, resource-contract tests, and deterministic verify;
- include visual sheets or representative outputs for human fidelity review.

Required generator checks are:

```sh
python3 scripts/build-modern-icons.py lint --complete
python3 scripts/build-modern-icons.py verify
python3 v2.5-beta-1-modern/tests/modern_icon_pipeline_test.py
python3 v2.5-beta-1-modern/tests/original_artwork_runtime_test.py
python3 v2.5-beta-1-modern/tests/source_strip_topology_test.py
python3 v2.5-beta-1-modern/tests/windows_icon_integration_test.py
```

A successful raster conversion says nothing about design fidelity. Review the
rendered sizes actually shipped, especially 16/20/24/32 px, expression strips,
and the MFC consumer's index mapping.

## 14. Performance is measured behavior

Do not optimize from aesthetics. State a hypothesis, name the user-visible or
resource metric, measure the baseline, change one cause, and compare the same
build type on the same machine. LLVM's [benchmarking guidance][llvm-benchmark]
emphasizes repeated runs and controlling noise.

Relevant metrics include UI-frame latency, event-loop idle/wakeup behavior,
connect/handshake latency, bytes and commands retained at saturation, render
batch allocation/count, text shaping/cache hit behavior, scheduler queue wait,
working set, and shutdown/restart latency. Throughput without bounded latency,
memory, fairness, and cancellation is not a win.

The repository microbenchmark target covers representative core operations:

```sh
meson test -C portable/build-agent --suite perf --verbose
```

Record compiler and version, commit, build type, machine/OS, repetitions, raw
samples or artifact path, median/distribution, and checksum. Do not compare a
sanitized debug baseline with an optimized release patch. Keep correctness
tests independent of benchmark thresholds unless the environment is controlled
enough to make a regression gate reliable.

Use PMR/arenas for bounded frame-lifetime allocation where reset semantics are
already explicit. Use compact immutable snapshots when they reduce lock time
and improve locality. Do not create an unbounded global cache, retain per-room
history indefinitely, oversubscribe workers, add speculative async hops, or use
lock-free structures without a demonstrated bottleneck and a reclamation proof.

## 15. Verification matrix and commands

Choose gates by the changed risk, then run from narrow to broad. Never report a
skipped, timed-out, or unavailable gate as passing.

The current native acceptance matrix is:

| Surface | Current CI toolchain | What constitutes proof |
|---|---|---|
| Linux portable/core | Ubuntu 24.04, pinned Clang 22.1.8, strict C++26 Meson | Release compile, affected/full tests, headless render, and sanitizers when risk requires them |
| Linux Wayland | SDL3 under a real headless Weston compositor | Native Wayland launch and non-empty rendered artifact, followed by visual inspection when UI changes |
| Linux X11 | SDL3 fallback | Add an explicit X11 runtime smoke when X11-specific code changes; Wayland or dummy success is not X11 proof |
| FreeBSD | FreeBSD 15.0 VM, Clang 22 | Native Meson compile, tests, and frontend render using the BSD libraries/runtime |
| OpenBSD | OpenBSD 7.9 VM, Clang 21 | Native Meson compile, tests, and frontend render, including the standard-library fallback paths |
| Windows | Windows VS2026 image, MSVC 14.51+, x86 MFC/NMAKE | Native dependency build, resource compile, MFC link, affected native tests, packaging, and random-folder launch smoke |

These versions describe the checked-in workflow, not a permanent promise. Read
`.github/workflows/build-modern.yml` before each handoff and record the exact
versions that ran.

### Portable strict build

From the repository root:

```sh
git submodule update --init --recursive
CC=clang CXX=clang++ meson setup portable/build-agent portable \
  --buildtype=release
meson compile -C portable/build-agent
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  meson test -C portable/build-agent --print-errorlogs
```

Use Meson's named tests for a causal iteration, for example:

```sh
meson test -C portable/build-agent comicchat-ircv3 --print-errorlogs
meson test -C portable/build-agent comicchat-transport --print-errorlogs
meson test -C portable/build-agent comicchat-dcc-transfer --print-errorlogs
meson test -C portable/build-agent comicchat-headless --print-errorlogs
```

Test names, options, and target membership come from `portable/meson.build`,
not memory. Meson's [test documentation][meson-tests] defines execution and log
behavior.

### Sanitizers and concurrency

Parser, asset decoder, transport, ownership, secret, and memory changes require
ASan+UBSan on the relevant suite and then the affected full set:

```sh
CC=clang CXX=clang++ meson setup portable/build-asan-agent portable \
  -Db_sanitize=address,undefined -Db_lundef=false -Dfrontend=false
meson compile -C portable/build-asan-agent
ASAN_OPTIONS=detect_leaks=1 \
  meson test -C portable/build-asan-agent --print-errorlogs
```

Concurrent/restart changes also require TSan and repeated lifecycle stress:

```sh
CC=clang CXX=clang++ meson setup portable/build-tsan-agent portable \
  -Db_sanitize=thread -Db_lundef=false -Dfrontend=false
meson compile -C portable/build-tsan-agent
meson test -C portable/build-tsan-agent --print-errorlogs
```

Clang documents [ASan][asan], [UBSan][ubsan], and [TSan][tsan]. A sanitizer
finding is a defect until its root cause is proven. Suppressions need the
smallest scope, an upstream issue or precise explanation, and a regression
guard. Sanitizers are test tools, not production runtime hardening.

When changing `thread_compat.hpp`, scheduling, or engine shutdown, create a
fresh build that defines `COMICCHAT_FORCE_THREAD_FALLBACK=1` and run the same
affected tests. Do not assume the host's standard-library feature selection
exercises the BSD fallback:

```sh
CC=clang CXX=clang++ CXXFLAGS=-DCOMICCHAT_FORCE_THREAD_FALLBACK=1 \
  meson setup portable/build-thread-fallback-agent portable \
  --buildtype=release -Dfrontend=false
meson compile -C portable/build-thread-fallback-agent
meson test -C portable/build-thread-fallback-agent --print-errorlogs
```

### Static analysis

The Meson setup emits `compile_commands.json`. Run `clang-tidy` against changed
first-party translation units using that database and an explicit, recorded
check set; this repository does not currently have a root `.clang-tidy`, so an
unspecified local default is not reproducible evidence. Add
`-Wunsafe-buffer-usage` or Clang [C++ Safe Buffers
analysis][clang-safe-buffers] as an audit signal for parser/decoder boundaries;
review C interop manually rather than blindly applying fix-its. Use
`-Wthread-safety` only after contracts are annotated accurately. On Windows,
MSVC `/analyze` is additional evidence for the MFC boundary, not a substitute
for Clang or runtime tests; see Microsoft's [C++ code analysis
documentation][msvc-analyze].

### UI and native platform gates

For portable UI/render changes, run a deterministic dummy-driver image and a
real Wayland compositor smoke. Inspect the image artifact; do not call file
existence visual proof.

For native Windows work, run from the supported Visual Studio x86 Developer
Command Prompt after the pinned libuv/mbedTLS libraries are staged exactly as
CI does:

```bat
pushd v2.5-beta-1-modern
nmake /K /f chat.mak CFG="chat - Win32 Release"
popd
```

Run all affected native C++ test executables and Python resource/UI audits that
the change touches. Then package and launch from random folders through the
repository smoke script. A successful NMAKE compile alone does not prove MFC
resource loading, dialog chrome, DPI behavior, or startup dependencies.

The integrated commit must pass the current `.github/workflows/build-modern.yml`
matrix. At present it provides strict Linux Clang plus real Wayland, native
FreeBSD and OpenBSD Clang builds, and native Windows MSVC/MFC build/package/
smoke coverage. The workflow file is the source of truth for exact OS and
toolchain versions. Cross-compilation, a container, or a dummy display cannot
be relabeled as the native gate.

Always finish with:

```sh
git diff --check
git status --short
git diff --stat
git diff -- path/to/every/in-scope/file
```

After integration, rerun affected causal tests and the complete required matrix
on the exact merged head. Topic-worktree evidence does not prove conflict
resolution or neighboring commits.

## 16. Review protocol: severity and reproducible evidence

Review in this order:

1. behavior/protocol/Microsoft-source correctness;
2. security, bounds, trust decisions, and secret lifetime;
3. ownership, callback lifetime, cancellation, restart, and concurrency;
4. shared Clang/MSVC and legacy adapter compatibility;
5. UI/resource fidelity and platform-native behavior;
6. causal test gaps and observability;
7. measured performance regressions;
8. maintainability that affects the preceding properties.

Use these severities:

- **P0 — release blocker:** exploitable secret/memory/trust failure, data loss,
  protocol downgrade, reproducible crash/deadlock, unbounded attacker-controlled
  growth, or required native lane broken.
- **P1 — correctness blocker:** wrong negotiated behavior, stale-generation
  delivery, lifecycle leak, source-fidelity regression, adapter mismatch, or a
  major failure path with no safe recovery.
- **P2 — important:** bounded edge-case defect, meaningful portability gap,
  missing causal coverage for risky logic, or measurable regression.
- **P3 — follow-up:** maintainability or clarity issue with a concrete future
  failure mode. Style preference alone is not a finding.

Every finding contains severity, `file:line`, violated invariant/spec/source,
concrete input or interleaving, user/security consequence, and the smallest
test or command that would prove it. Distinguish confirmed defects from
hypotheses. Read the actual patch and surrounding code; do not review only a
handoff summary. A fresh reviewer should challenge arithmetic, negative paths,
callback destruction, and platform assumptions before commenting on naming.

No author supplies the sole final review of their own security, parser, crypto,
transport, concurrency, or rendering-fidelity patch. Cross-model review is an
independent perspective, not validation by consensus. Codex or CI reproduces
release-blocking claims with tools.

## 17. Repository-specific anti-patterns

Reject these patterns during implementation or review:

- editing `/home/kain/comicchat` while intending to change this legacy fork;
- modifying Microsoft snapshot sources instead of porting from them;
- declaring visual fidelity from a screenshot resemblance or placeholder;
- hand-editing generated icons/resources or omitting NMAKE dependencies;
- using compiler version alone as a C++26 feature probe;
- landing shared `portable/` API changes after only a Clang build;
- wrapping every raw MFC observer in `shared_ptr` without ownership analysis;
- freeing libuv handle storage before `uv_close_cb` or treating requests as
  handles;
- calling loop/handle APIs from arbitrary threads or invoking `uv_run` inside a
  callback;
- detaching a worker, self-joining, or destroying an engine from its callback;
- holding an internal lock across notifier/UI callbacks or thread join;
- accepting a callback from an old generation after restart;
- unbounded strings, batches, queues, caches, retry lists, or render histories;
- checking a bound after addition/multiplication/allocation;
- treating `WANT_READ`/`WANT_WRITE` as fatal or spinning until TLS progresses;
- plaintext fallback after TLS failure;
- logging passwords, AUTHENTICATE payloads, SCRAM material, or sensitive queued
  bytes;
- wiping only heap storage while leaving SSO source/intermediate copies;
- auto-requesting an IRCv3 capability before the legacy consumer adapts it;
- confusing spec wire bytes with decoded payload bytes or display characters;
- claiming async/multicore/PMR/lock-free performance without a baseline;
- hiding a test failure with a larger timeout, disabled warning, or broad
  sanitizer suppression;
- citing “all tests pass” without exact commands, exit codes, and head commit;
- treating skipped native CI or a cross-syntax compile as platform proof.

## 18. C++ handoff

Use the repository handoff schema in `docs/AI-DEVELOPMENT-WORKFLOW.md`. Before
its standard `HANDOFF` block, include this C++ evidence summary:

```text
C++ EVIDENCE
change_graph: declarations, definitions, consumers, adapters, build targets
original_or_spec_oracle: exact paths/symbols/URLs
ownership: owner, observer, allocation, cleanup, last callback
threads: UI/loop/worker access, mutex/atomic rule, generation rule
bounds: input, item/byte/work limits, overflow checks, backpressure outcome
secrets: copies, locks, wipe points, diagnostics policy
cpp26: language mode plus every new feature-test macro/fallback
platforms: Clang portable, MSVC/MFC, Linux/Wayland, FreeBSD, OpenBSD impact
performance: hypothesis and before/after evidence, or not applicable with reason
END C++ EVIDENCE
```

Then provide the full base/head/commit, clean status, diffstat, fingerprint,
RED/GREEN commands, sanitizer/static-analysis/platform results, artifacts,
findings, residual risks, and one next action. Never write “not applicable” for
a gate without the reason tied to the changed risk. Never report an unexecuted
command as evidence.

## Official primary references

- WG21 current C++26 draft and feature testing: [N5046][cpp26-draft],
  [SD-6][sd6]
- Compiler/build: [Clang C++ status][clang-cxx-status], [Meson built-in
  options][meson-options], [Meson tests][meson-tests], [MSVC `/std`][msvc-std],
  [NMAKE][nmake]
- C++ design and analysis: [C++ Core Guidelines][core-guidelines], [Clang
  Thread Safety Analysis][clang-thread-safety], [Clang C++ Safe
  Buffers][clang-safe-buffers]
- Dynamic verification: [ASan][asan], [UBSan][ubsan], [TSan][tsan],
  [libFuzzer][libfuzzer]
- Async I/O: [libuv design][libuv-design], [loop API][libuv-loop],
  [cancellation guide][libuv-cancel]
- TLS and secrets: [Mbed TLS API][mbedtls-api], [porting/nonblocking
  BIO][mbedtls-porting], [secure zeroize][mbedtls-zeroize], [security
  advisories][mbedtls-advisories]
- Authentication/protocol: [RFC 4422][rfc4422], [RFC 5802][rfc5802],
  [RFC 7677][rfc7677], [IRCv3 specifications][ircv3-index]
- Native UI: [SDL main-thread API][sdl-main-thread], [SDL Wayland
  notes][sdl-wayland], [MFC threading][mfc-threading]
- Measurement: [LLVM benchmarking guidance][llvm-benchmark]

[asan]: https://clang.llvm.org/docs/AddressSanitizer.html
[clang-cxx-status]: https://clang.llvm.org/cxx_status.html
[clang-safe-buffers]: https://clang.llvm.org/docs/SafeBuffers.html
[clang-thread-safety]: https://clang.llvm.org/docs/ThreadSafetyAnalysis.html
[core-concurrency]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html#S-concurrency
[core-guidelines]: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines.html
[cpp26-draft]: https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/n5046.pdf
[ircv3-index]: https://ircv3.net/irc/
[ircv3-multiline]: https://ircv3.net/specs/extensions/multiline
[ircv3-sasl]: https://ircv3.net/specs/extensions/sasl-3.2
[ircv3-sts]: https://ircv3.net/specs/extensions/sts
[ircv3-tags]: https://ircv3.net/specs/extensions/message-tags.html
[libfuzzer]: https://llvm.org/docs/LibFuzzer.html
[libuv-cancel]: https://docs.libuv.org/en/stable/guide/threads.html
[libuv-design]: https://docs.libuv.org/en/stable/design.html
[libuv-loop]: https://docs.libuv.org/en/stable/loop.html
[llvm-benchmark]: https://llvm.org/docs/Benchmarking.html
[mbedtls-advisories]: https://mbed-tls.readthedocs.io/en/latest/security-advisories/
[mbedtls-api]: https://mbed-tls.readthedocs.io/projects/api/en/development/
[mbedtls-porting]: https://mbed-tls.readthedocs.io/en/latest/kb/how-to/how-do-i-port-mbed-tls-to-a-new-environment-OS/
[mbedtls-zeroize]: https://mbed-tls.readthedocs.io/projects/api/en/development/api/file/platform__util_8h/
[meson-options]: https://mesonbuild.com/Builtin-options.html
[meson-tests]: https://mesonbuild.com/Unit-tests.html
[mfc-threading]: https://learn.microsoft.com/en-us/cpp/parallel/multithreading-with-cpp-and-mfc?view=msvc-170
[msvc-analyze]: https://learn.microsoft.com/en-us/cpp/code-quality/code-analysis-for-c-cpp-overview?view=msvc-170
[msvc-std]: https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version?view=msvc-170
[nmake]: https://learn.microsoft.com/en-us/cpp/build/reference/nmake-reference?view=msvc-170
[rfc4422]: https://www.rfc-editor.org/rfc/rfc4422.html
[rfc5802]: https://www.rfc-editor.org/rfc/rfc5802.html
[rfc7677]: https://www.rfc-editor.org/rfc/rfc7677.html
[sd6]: https://isocpp.org/std/standing-documents/sd-6-sg10-feature-test-recommendations
[sdl-main-thread]: https://wiki.libsdl.org/SDL3/SDL_IsMainThread
[sdl-wayland]: https://wiki.libsdl.org/SDL3/README-wayland
[tsan]: https://clang.llvm.org/docs/ThreadSanitizer.html
[ubsan]: https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html
