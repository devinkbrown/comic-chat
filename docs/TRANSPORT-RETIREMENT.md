# IRC transport retirement audit

Audit date: 2026-07-16
Audited revision: `1dc102030035d5cdf55e9f4d62b7cc5b2c77093b`

> **Status update (2026-07-17): superseded on later revisions.** This is retained
> as the 2026-07-16 snapshot of the pinned revision above. Two of its central
> negative findings no longer hold at the current tree (`0f1f9f8`):
>
> - **v1 is migrated.** `v1.0-pre-modern` no longer runs the MFC/WinSock stack.
>   `CIrcSocket` is now a plain `final` class that owns a
>   `comicchat::net::ConnectionEngine` (`v1.0-pre-modern/ircsock.h:25,61`); there
>   is no `CAsyncSocket`, `AfxSocketInit`, `m_hSocket`, or inherited `Receive`
>   left in `v1.0-pre-modern/irc.cpp`, and its NMAKE build compiles
>   `connection_engine`/`ircv3`/`crypto_runtime` and links libuv/mbedTLS
>   (`v1.0-pre-modern/chat.mak:147-152`, `v1.0-pre-modern/chat.mak:114`). See
>   commit `791f2c9` ("Migrate v1 IRC to shared connection engine") and its
>   STS/membership follow-ups.
> - **The portable frontend is integrated.** `portable/src/app.cpp` now creates a
>   `NativeSession`/`ConnectionEngine` and parses
>   `--connect HOST [PORT] NICK CHANNEL [--tls|--plaintext]`
>   (`portable/src/app.cpp:82`, `portable/src/app.cpp:211-223`), so the "IRC
>   runtime absent" finding for the SDL frontend is also out of date.
>
> Read the sections below as the historical migration plan and RED baseline, not
> the present state; the completion checklist is now largely satisfied.

## Decision

The old TCP stack is retired for IRC in `v2.5-beta-1-modern`, but it is still the
only live IRC transport in `v1.0-pre-modern`. The shared portable transport is
well tested as a library, but the SDL frontend does not yet create an IRC
session. Therefore the repository does **not** currently satisfy the project
architecture stated in `AGENTS.md:18-20`.

Treat this as a release blocker for any claim that both legacy clients have been
ported to the shared network stack.

The target invariant is:

> Every modern IRC connection, reconnect, read, write, proxy exchange, and TLS
> handshake goes through `comicchat::net::ConnectionEngine`. libuv owns DNS,
> connect, readiness, timers, cancellation, and loop lifetime; mbedTLS owns TLS.
> No modern client can create or fall back to an MFC/WinSock IRC transport.
> Plaintext is an explicit compatibility mode carried by the same engine, never
> a recovery path after TLS failure.

This invariant is about IRC server I/O. DCC uses its separately bounded
`DccTransferEngine`, and the Windows HTTP artwork subsystem uses WinINet; neither
is an alternate IRC transport.

## Audit method and scope

The Microsoft trees `v1.0-pre/` and `v2.5-beta-1/` were read as immutable
behavioral references, as required by `AGENTS.md:10-20`. The active trees were
searched for MFC socket inheritance and initialization, socket handles, direct
socket calls, legacy `Create`/`Connect`/`Send`/`Receive` callbacks, shared-engine
ownership, TLS selection, build objects, tests, and CI execution.

Negative findings below mean a repository-wide search of the stated modern tree
at the audited revision, not an inference from one file. Test-only loopback
servers were classified separately from product code.

## What Microsoft originally shipped

### v1.0 prerelease oracle

The original `CIrcSocket` is directly derived from `CAsyncSocket` and exposes
MFC `OnConnect`, `OnClose`, and `OnReceive` callbacks
(`v1.0-pre/client/ircsock.h:1-7`). Startup calls `AfxSocketInit`, inspects the
public MFC socket handle, calls `Create`, and accepts `WSAEWOULDBLOCK` from
`Connect` (`v1.0-pre/client/irc.cpp:135-172`). The callback performs IRC login
by sending `NICK` and `USER` directly (`v1.0-pre/client/irc.cpp:963-990`). This
is the compatibility behavior that the modern adapter must preserve at the
protocol/UI boundary, not the implementation to retain.

### v2.5 beta 1 oracle

The original v2.5 client also derives `CIrcSocket` from `CAsyncSocket`
(`v2.5-beta-1/ircsock.h:438-459`) and receives bytes through the inherited
`Receive` call (`v2.5-beta-1/ircsock.cpp:1000-1032`). Its server-selection layer
adds another MFC socket class (`v2.5-beta-1/chatsrv.h:281-297`), connects several
candidate sockets with `CAsyncSocket::Connect`
(`v2.5-beta-1/chatsrv.cpp:1766-1781`), then transfers the winning raw handle into
the global socket and manually invokes its callback
(`v2.5-beta-1/chatsrv.cpp:1801-1828`,
`v2.5-beta-1/chatsrv.cpp:1841-1890`). The direct fallback connect path likewise
calls `serverConn.Create()` and `serverConn.Connect()`
(`v2.5-beta-1/protsupp.cpp:4826-4828`).

The modern v2.5 implementation correctly removed both the handle-transfer
mechanism and the fallback connector. The original source is still valuable for
server-selection and callback ordering, but not for socket ownership.

## Current product paths

### `v2.5-beta-1-modern`: shared transport active, cleanup incomplete

Status: **IRC migration implemented; residual compatibility and policy work
remain.**

Evidence for the live path:

1. `CIrcSocket` is now a plain class, owns one `ConnectionEngine`, and carries a
   generation, line framer, and bounded adapter event state
   (`v2.5-beta-1-modern/ircsock.h:454-480`,
   `v2.5-beta-1-modern/ircsock.h:516-553`). There is no `CAsyncSocket` in this
   modern tree.
2. `StartConnection` constructs `ConnectionOptions`, explicitly selects
   `Security::tls` or `Security::plaintext`, installs a generation-cookie UI
   wakeup, and starts the shared engine
   (`v2.5-beta-1-modern/ircsock.cpp:613-666`). It never constructs an MFC socket.
3. The legacy-named `Send` method is only an adapter facade. It validates and
   prepares an IRC line, creates a generation-tagged `comicchat::net::Send`, and
   posts it to the bounded engine queue
   (`v2.5-beta-1-modern/ircsock.cpp:688-733`).
4. The UI thread drains `ConnectionEngine` events. `Connected` calls the old
   semantic login handler; `BytesReceived` goes through `LineFramer` and the
   IRCv3 engine; `Closed` becomes the legacy UI disconnect signal
   (`v2.5-beta-1-modern/ircsock.cpp:773-875`). The main frame only receives a
   cookie wakeup and drains these events on the UI thread
   (`v2.5-beta-1-modern/mainfrm.cpp:60-73`).
5. The only protocol-layer call into the legacy-named facade is
   `CIrcProto::SendMessageText` (`v2.5-beta-1-modern/ircproto.cpp:465-472`), plus
   one internal authentication response in `CIrcSocket`
   (`v2.5-beta-1-modern/ircsock.cpp:1698-1711`). Neither is a direct socket send.

Residual items and their classification:

| Surface | Evidence | Classification |
| --- | --- | --- |
| `AfxSocketInit()` | `v2.5-beta-1-modern/ircproto.cpp:47-59` | Active startup dependency left from MFC sockets; no longer an IRC I/O path, but must be removed and regression tested. |
| `<afxsock.h>` | `v2.5-beta-1-modern/ircproto.cpp:1-3`; `v2.5-beta-1-modern/stdafx.h:23` | Compile-time residue. |
| `serverConn.Connect` / `CIrcSocket::Connect` | `v2.5-beta-1-modern/protsupp.cpp:4772-4787`; `v2.5-beta-1-modern/ircsock.cpp:613-666` | Active compatibility facade that constructs options and calls `ConnectionEngine::start`; it does not invoke MFC/WinSock connect. |
| `serverConn.Close` / `CIrcSocket::Close` | `v2.5-beta-1-modern/protsupp.cpp:4683-4688`; `v2.5-beta-1-modern/ircsock.cpp:669-681` | Active compatibility facade over `ConnectionEngine::stop`, not a socket-handle close. |
| `CIrcSocket::Send` | `v2.5-beta-1-modern/ircsock.cpp:688-733` | Active compatibility facade over `ConnectionEngine::post`, not a legacy TCP send. Rename only after all callers use a transport-neutral verb. |
| `CIrcSocket::OnConnect` | `v2.5-beta-1-modern/ircsock.cpp:1059-1108` | Active legacy semantic handler invoked by a typed `Connected` event, not an MFC callback. |
| `CIrcSocket::OnClose` | `v2.5-beta-1-modern/ircsock.cpp:1111-1116` | Active legacy UI/error handler invoked by adapter failures/events, not an MFC callback. |
| `SOCKET_ERROR` and `WSA*` constants | `v2.5-beta-1-modern/ircsock.cpp:688-700`, `v2.5-beta-1-modern/ircsock.cpp:802-861` | Windows-compatible return/error vocabulary only. No handle operation occurs here. |

There is no active `OnReceive`, `m_hSocket`, `serverConn.Create`, or direct
`socket`, system `connect`, system `send`, or system `recv` in the v2.5 modern
client tree. Its `serverConn.Connect` name is the engine facade classified above.

The main remaining security-policy gap is selection, not fallback. The UI model
still defaults server records to port 6667 (`v2.5-beta-1-modern/chatsrv.h:69-72`,
`v2.5-beta-1-modern/chatsrv.cpp:280-289`) and treats only port 6697 as TLS
(`v2.5-beta-1-modern/protsupp.cpp:4772-4787`). A TLS server on a nonstandard port
is therefore treated as plaintext, and a plaintext service on 6697 is treated as
TLS. TLS must become explicit persisted server policy, with secure defaults;
port number is not a security setting.

Once TLS is selected, the shared engine does fail closed. It starts plaintext
only when `Security::plaintext` was supplied
(`portable/src/net/connection_engine.cpp:1119-1129`), verifies the TLS result
before publishing a secure connection
(`portable/src/net/connection_engine.cpp:1234-1269`), and retries with the same
unchanged `options_` after failure
(`portable/src/net/connection_engine.cpp:1495-1513`). No TLS-error branch changes
the connection to plaintext.

### `v1.0-pre-modern`: old transport fully active

Status: **not migrated; invariant fails.**

The current class still derives from `CAsyncSocket` and declares all three MFC
callbacks (`v1.0-pre-modern/ircsock.h:4-10`). The active connection lifecycle is
the original implementation:

- Startup calls `AfxSocketInit` (`v1.0-pre-modern/irc.cpp:139-145`).
- Reconnect/disconnect inspect the public `m_hSocket` and call the inherited
  `Close` (`v1.0-pre-modern/irc.cpp:147-176`,
  `v1.0-pre-modern/irc.cpp:480-492`).
- Connect calls inherited `Create` and `Connect`, treating `WSAEWOULDBLOCK` as
  asynchronous success (`v1.0-pre-modern/irc.cpp:172-183`).
- `OnReceive` calls inherited `Receive` into one static 513-byte buffer and
  performs its own newline splitting (`v1.0-pre-modern/irc.cpp:958-989`).
- `OnConnect` sends `NICK` and `USER` directly
  (`v1.0-pre-modern/irc.cpp:992-1020`); `OnClose` only changes UI state
  (`v1.0-pre-modern/irc.cpp:1022-1025`).

Every active old `Send` call is listed here. This is the complete ledger at the
audited revision:

| Wire purpose | Active call evidence |
| --- | --- |
| `PART` | `v1.0-pre-modern/irc.cpp:480-484` |
| Avatar announcement `PRIVMSG` | `v1.0-pre-modern/irc.cpp:508-514` |
| Profile response `PRIVMSG` | `v1.0-pre-modern/irc.cpp:530-540` |
| Multi-channel `LIST` | `v1.0-pre-modern/irc.cpp:713-734` |
| `PONG` | `v1.0-pre-modern/irc.cpp:819-829` |
| Initial `NICK` | `v1.0-pre-modern/irc.cpp:992-1011` |
| Initial `USER` | `v1.0-pre-modern/irc.cpp:1011-1019` |
| Chat/action/whisper `PRIVMSG` | `v1.0-pre-modern/irc.cpp:1090-1113` |
| Profile request `PRIVMSG` | `v1.0-pre-modern/irc.cpp:1119-1126` |
| Room `LIST` / user `WHOIS` | `v1.0-pre-modern/irc.cpp:1175-1186` |
| `KICK` | `v1.0-pre-modern/irc.cpp:1226-1239` |
| `TOPIC` | `v1.0-pre-modern/irc.cpp:1242-1248` |
| Nick change `NICK` | `v1.0-pre-modern/irc.cpp:1285-1295` |
| `JOIN` | `v1.0-pre-modern/irc.cpp:1301-1304` |
| Operator `MODE` | `v1.0-pre-modern/irc.cpp:1346-1350` |

The apparent ring `Send` at `v1.0-pre-modern/irc.cpp:1129-1133` is inside
`#if 0` through line 1151 and is not active.

This client has no TLS at runtime. Its setup dialog defaults to port 6667 and
has no security field (`v1.0-pre-modern/setupdlg.cpp:41-54`,
`v1.0-pre-modern/setupdlg.h:63-80`). `tlssock.cpp` and `tlssock.h` are an
unintegrated SChannel experiment: no product source includes them, and neither
release nor debug object lists contain `tlssock.obj`
(`v1.0-pre-modern/chat.mak:138-188`,
`v1.0-pre-modern/chat.mak:298-348`). Even that experiment is unsuitable as the
target because its design document records permissive certificate validation
(`docs/tls.md:101-107`). It must not be activated as an interim fallback.

### Portable Linux/BSD frontend: substrate present, IRC runtime absent

Status: **shared engine and tests exist; frontend integration is not started.**

Meson builds `connection_engine.cpp`, `dcc_transfer_engine.cpp`, and `ircv3.cpp`
into the shared core with libuv and pinned mbedTLS dependencies
(`portable/meson.build:261-312`). The executable links that core
(`portable/meson.build:360-369`), but `portable/src/app.cpp` imports only render
and text APIs and parses only font/PNG/frame arguments
(`portable/src/app.cpp:1-20`, `portable/src/app.cpp:38-75`). It never creates a
`ConnectionEngine`, parses `ConnectionConfig`, or starts an IRC registration.

There is consequently no alternate Unix socket stack to retire, but also no
working Unix/BSD IRC client path yet. Do not count library linkage as frontend
transport integration.

## Security consequences of the split state

| Priority | Finding | Evidence and consequence |
| --- | --- | --- |
| P0 | v1 has no authenticated encryption | Every v1 IRC byte uses the MFC plaintext path; the UI defaults to 6667 and exposes no TLS policy (`v1.0-pre-modern/setupdlg.cpp:41-54`). Network observers can read and modify sessions. |
| P0 | v1 indexes the receive buffer with an unchecked return value | `Receive` is followed immediately by `startPtr[nRead] = '\0'` (`v1.0-pre-modern/irc.cpp:960-969`). An inherited socket error can return `SOCKET_ERROR`, making this a write before `startPtr`; a full unterminated line also drives the remaining capacity to zero. Do not copy this loop into the adapter. |
| P1 | v1 carries framing state across connection generations | The receive accumulator is a function-static buffer and is not cleared by `OnClose` (`v1.0-pre-modern/irc.cpp:958-989`, `v1.0-pre-modern/irc.cpp:1022-1025`). A partial line from one server can survive into another connection. |
| P1 | v1 has no bounded asynchronous admission layer | Fifteen callers write through inherited `Send`, while the fixed global output buffer is only 513 bytes (`v1.0-pre-modern/irc.cpp:50-53`). There is no generation validation, queue capacity, priority, or backpressure contract at this boundary. |
| P1 | v1 ignores every send result | Callers discard the inherited nonblocking `Send` result, including chat output (`v1.0-pre-modern/irc.cpp:1100-1113`) and registration (`v1.0-pre-modern/irc.cpp:1008-1019`). A short write or would-block result can truncate an IRC command with no retry or terminal error. |
| P1 | v2 security is inferred from a conventional port | `secure` is exactly `port == 6697` (`v2.5-beta-1-modern/protsupp.cpp:4772-4783`). The engine itself does not downgrade, but the caller can select the wrong mode before the engine starts. |
| P1 | CI does not prove exclusive runtime ownership | Clang checks the core API and Windows launches a GUI, but neither test proves which transport a legacy executable uses. A future MFC fallback could compile and ship unnoticed. |

The migration should preserve the original UI and wire semantics while deleting
these ownership and lifetime properties. It should not preserve the exact
buffering, callback reentrancy, or error handling merely because they are
historical behavior.

## Shared engine socket boundary

Low-level system calls are intentionally confined to the shared implementation.
libuv performs DNS and TCP connect (`portable/src/net/connection_engine.cpp:765-795`,
`portable/src/net/connection_engine.cpp:846-899`) and owns readiness polling
(`portable/src/net/connection_engine.cpp:726-753`). The engine obtains libuv's
connected socket so mbedTLS can use a nonblocking BIO. TLS reads/writes go through
`mbedtls_ssl_read` and `mbedtls_ssl_write`; explicit plaintext uses the same
bounded scheduler and raw BIO helpers
(`portable/src/net/connection_engine.cpp:1272-1318`,
`portable/src/net/connection_engine.cpp:1388-1439`). The only product IRC
`::send`, `::recv`, and `getsockname` calls are that allowlisted implementation
boundary (`portable/src/net/connection_engine.cpp:1592-1647`).

Direct socket calls in `portable/tests/transport_test.cpp` belong to hostile and
loopback test servers, not the client transport. DCC is separately implemented
with libuv and exposed as a distinct API; its compile surface is checked beside
the connection engine (`v2.5-beta-1-modern/tests/transport_adapter_api_compile.cpp:1-54`).

## Build, test, and CI coverage

### What is wired correctly

- The v2.5 NMAKE build links `libuv`, `mbedtls`, `mbedx509`, and `mbedcrypto`
  (`v2.5-beta-1-modern/chat.mak:119-126`) and compiles the shared connection,
  DCC, IRCv3, crypto-runtime, and adapter-API objects
  (`v2.5-beta-1-modern/chat.mak:169-177`,
  `v2.5-beta-1-modern/chat.mak:247-266`).
- The portable build compiles the adapter API against Clang as well as MSVC
  (`portable/meson.build:314-321`).
- Portable loopback tests prove bounded plaintext I/O, mbedTLS I/O, hostname
  mismatch failure, handshake deadlines, queue limits, proxies, reconnect,
  session reuse, and clean shutdown; representative anchors are
  `portable/tests/transport_test.cpp:744-845`,
  `portable/tests/transport_test.cpp:958-1009`, and
  `portable/tests/transport_test.cpp:1201-1273`.
- Linux, FreeBSD, and OpenBSD CI compile and run all Meson tests
  (`.github/workflows/build-modern.yml:27-80`,
  `.github/workflows/build-modern.yml:109-141`,
  `.github/workflows/build-modern.yml:146-179`). Windows builds the pinned
  libuv/mbedTLS libraries before both legacy clients
  (`.github/workflows/build-modern.yml:199-261`).

### What is missing or misleading

- The v1 NMAKE build contains only the old `irc.obj` network path and links no
  shared transport or mbedTLS objects/libraries
  (`v1.0-pre-modern/chat.mak:131-188`,
  `v1.0-pre-modern/chat.mak:291-348`). The fact that CI builds libuv/mbedTLS first
  does not make v1 consume them.
- Windows CI builds both clients but runs no IRC loopback, TLS, reconnect, or
  adapter test against either executable (`.github/workflows/build-modern.yml:261-302`).
  The package smoke test proves only that each PE creates a window and remains
  alive for several seconds (`scripts/smoke-test-modern-builds.ps1:58-83`).
- `transport_adapter_api_compile.cpp` verifies the shared API signature, not
  that either MFC client actually owns or exclusively uses it
  (`v2.5-beta-1-modern/tests/transport_adapter_api_compile.cpp:11-54`).
- There is no static source gate forbidding a reintroduced MFC/direct IRC socket
  or asserting that both Windows makefiles link the shared objects.
- Repository narrative is stale. The v2.5 README still says TLS is absent
  (`v2.5-beta-1-modern/README.md:137-143`), while the current v2.5 source uses
  mbedTLS. `docs/tls.md:9-13` describes a wired SChannel branch that is not wired
  on this revision. Source and executable tests must remain the truth source.

## Required causal proof

Do not accept source deletion alone as retirement. Add these gates before or in
the migration commits.

### Implemented inventory gate

`scripts/check-transport-ownership.py` implements the first static gate in the
temporary-allowlist phase. It makes every v2.5 MFC/direct-network regression
fatal, confines low-level socket/libuv/mbedTLS transport calls to the shared
connection and DCC engine implementations, and verifies active NMAKE object
lists, compile rules, include paths, and link libraries in both Release and
Debug configurations. Comments and literal `!IF 0` branches cannot satisfy a
build assertion.

The still-active v1 runtime stack is not hidden behind a directory exemption.
All 28 source findings are pinned by rule, file, line, and normalized active
code line. The v1 build must now satisfy all 17 shared-substrate checks in both
Release and Debug; a partial or commented-out migration is fatal. Any source
addition, removal, relocation, or substitution fails until the runtime
migration commit explicitly reduces the temporary inventory. The causal
negative fixtures live in `scripts/tests/test_transport_ownership.py`, and all
four modern CI lanes execute the gate, its tests, and the dedicated v1 C++26
substrate verifier.

### 1. Source ownership gate

Add a repository script and run it in every modern CI lane. It must:

- reject `CAsyncSocket`, `AfxSocketInit`, `m_hSocket`, inherited
  `Create`/`Connect`/`Receive`, and direct socket syscalls in both `*-modern/`
  trees;
- reject product `socket`/`connect`/`send`/`recv` outside the allowlisted shared
  connection and DCC engine implementations;
- assert both Windows makefiles compile `connection_engine`, `crypto_runtime`,
  `ircv3`, and an adapter contract object and link pinned libuv/mbedTLS;
- reject `tlssock.obj`, SChannel transport activation, or any conditional
  "try TLS, then plaintext" path;
- ignore immutable snapshots and direct calls in loopback test servers so the
  gate measures product ownership rather than historical/test fixtures.

The first version of this gate is the causal RED for v1. It should report exact
file and line matches and turn GREEN only after the v1 adapter is migrated.

### 2. Adapter contract tests for each client

Extract the small transport-to-UI policy seam far enough to test without a live
window. For **both** v1 and v2, prove:

- default configuration requests TLS and port 6697;
- plaintext requires an explicit persisted compatibility choice;
- a TLS configuration, certificate, hostname, handshake, proxy, or reconnect
  failure never starts a plaintext generation;
- exactly one current-generation `Connected` event starts legacy registration;
- stale-generation events cannot mutate UI/protocol state;
- split and coalesced input frames reach the old parser exactly once per line;
- every legacy outbound category reaches `ConnectionEngine::post`, is bounded,
  and preserves control/authentication/PONG priority;
- disconnect, window destruction, callback re-entry, and reconnect converge on
  one idempotent stop path.

Use a fake transport interface for deterministic adapter tests and keep the real
`ConnectionEngine` final/non-mockable. The real engine remains covered by its
loopback suite.

### 3. Native Windows loopback gate

Run an x86 Windows test binary built with the same headers, flags, and static
libraries as the MFC clients. It must exercise plaintext opt-in, a trusted local
TLS connection, hostname mismatch, handshake timeout, reconnect without
downgrade, send/receive framing, and clean shutdown. A GUI-start smoke is not a
network test.

### 4. Existing full and sanitizer gates

After focused RED/GREEN tests, run `comicchat-transport`, `comicchat-ircv3`, and
the full Meson suite. Run ASan+UBSan for buffers/TLS/parsers and TSan with repeated
connect/stop/restart and UI-wakeup stress. Require Linux, Wayland, FreeBSD,
OpenBSD, and native Windows jobs on the exact integrated commit.

## Smallest safe implementation sequence

1. **Land the static inventory gate in expected-failure mode for v1 only.** Make
   v2 regressions fatal immediately. Record the exact v1 findings above as the
   temporary allowlist; do not use a broad directory exemption.
2. **Make v1 link the shared substrate without changing behavior.** Modernize its
   NMAKE dependency/flag surface, compile `crypto_runtime`, `connection_engine`,
   `ircv3`, and the adapter contract, and link the same pinned libuv/mbedTLS
   libraries as v2. Add the Windows adapter test target first.
3. **Introduce a v1 non-MFC adapter behind the existing call shape.** Let a plain
   `CIrcSocket` own `ConnectionEngine`, a generation, wakeup cookie, `LineFramer`,
   and bounded events. Initially preserve `Send`, `OnConnect`, and `OnClose` as
   semantic facades so the 15 protocol callers and UI behavior do not change in
   the same patch. Route `Send` to `post`, and route typed engine events to the UI
   thread through one posted message.
4. **Replace v1 connect and receive ownership.** Remove `Create`, inherited
   `Connect`, `m_hSocket`, `Receive`, and `OnReceive`. Start
   `ConnectionOptions` through the shared engine; feed received bytes through
   `LineFramer` and the shared IRCv3 policy before dispatching compatible lines
   to the existing parser. Preserve original callback ordering with causal tests.
5. **Make transport security explicit in both legacy UIs.** Add a persisted TLS
   choice defaulting to TLS/6697 and a separately labeled explicit plaintext
   compatibility choice. Do not infer security from port. Apply STS before any
   insecure control/authentication output and never retain a downgrade after TLS
   failure.
6. **Delete the dead and residual stack.** Remove v1 `CAsyncSocket` inheritance,
   both `AfxSocketInit` calls and `<afxsock.h>` dependencies, the unused SChannel
   experiment, and obsolete socket-handle/error vocabulary once call sites use
   transport-neutral results. Keep `ws2_32` only where libuv/mbedTLS require it,
   not as evidence of a client-owned socket.
7. **Wire the portable frontend as a real consumer.** Add the IRC session model
   and event-to-UI bridge using the same `ConnectionEngine`; do not introduce a
   platform-specific Unix socket client. Reuse the explicit TLS/plaintext policy
   and adapter contract.
8. **Remove the temporary v1 allowlist and enforce the final gate everywhere.**
   Run focused, full, sanitizer, native-platform, package, and local TLS loopback
   tests on the exact commit. Update stale TLS/modernization documentation only
   after executable evidence is green.

## Completion checklist

Transport retirement is complete only when all are true:

- both legacy modern clients own `ConnectionEngine` and no MFC socket;
- the portable frontend creates its IRC session through the same engine;
- no modern client tree contains an active legacy/direct IRC socket path;
- TLS is the default, plaintext is explicit, and failure never downgrades;
- adapter, shared loopback, native Windows loopback, sanitizer, and all native
  platform jobs pass on the integrated revision;
- static gates make reintroduction of the retired stack a CI failure.
