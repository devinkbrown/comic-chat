---
name: comicchat-transport-security
description: Harden and verify Comic Chat's shared libuv and mbedTLS networking, TLS/proxy/reconnect state, credential handling, SASL/STS, flood control, CTCP/SOUND, and DCC transfer paths. Use when changing connection_engine, dcc_transfer_engine, IRC authentication, secret lifetime, async cancellation/restart, queue bounds, timeouts, transport adapters, or related security tests.
---

# Comic Chat transport security

## Establish the threat contract

1. Read AGENTS.md, portable/README.md, and references/threat-model.md.
2. Trace the real path through the public header, implementation, MFC adapter, and test. Do not reason from one layer in isolation.
3. State the attacker-controlled inputs, owned resources, secret material, cancellation points, hard limits, and fail-closed result.
4. Add a causal regression that fails on the baseline before changing the implementation.
5. Use comicchat-ircv3-compat alongside this skill when CAP, tags, batches, SASL wire state, STS, or server-profile semantics change.
6. Read `docs/TRANSPORT-RETIREMENT.md` when changing a modern client connection path, adapter, build object, or network CI gate. Re-audit both legacy clients rather than assuming shared-library linkage proves runtime use.

## Preserve the connection engine

- Keep one libuv loop and its handles owned by one joinable RAII worker. Marshal cross-thread work through the bounded command queue and uv_async_t wakeup.
- Keep all modern IRC DNS, connect, reconnect, read, write, proxy, and TLS work behind `ConnectionEngine`. Forbid `CAsyncSocket`, `AfxSocketInit`, client-owned socket handles, direct socket calls, and an alternate SChannel transport in modern client trees. Direct BIO syscalls belong only inside the allowlisted shared engine implementation.
- Keep every command and event generation-tagged. Reject stale work before it mutates live state or consumes bounded capacity.
- Make stop, restart, callback re-entry, wakeup exceptions, and partial initialization converge on one idempotent teardown path.
- Close libuv handles on the loop thread and join from an external owner. Treat a stop requested from the worker as request-only; never self-join or detach.
- Reserve terminal and diagnostic event capacity so saturation cannot hide completion or failure.
- Preserve bounded receive, transmit, command, event, proxy, TLS-session, DCC, and batch storage. Reject oversized work before expensive allocation or arithmetic.
- Keep PONG, authentication, and control traffic ahead of chat and bulk traffic while retaining per-target fairness and backpressure.
- Keep idle sockets event-driven. A timer spin or polling loop is a performance and battery regression.

## Preserve TLS and proxies

- Default to Security::tls and port 6697. Permit plaintext only through an explicit caller option; never downgrade after DNS, proxy, handshake, certificate, or reconnect failure.
- Persist transport security as an explicit setting. Never infer TLS solely from port 6697, and never treat another port as implicitly plaintext.
- Configure trusted roots, required peer verification, SNI, and mbedtls_ssl_set_hostname for the original IRC hostname before exchanging application bytes.
- Continue verifying the IRC endpoint after SOCKS5 or HTTP CONNECT. Do not authenticate the proxy hostname as the TLS peer.
- Validate proxy lengths, methods, status lines, IPv6 authority brackets, credentials, and response bounds before use.
- Reset every per-socket mbedTLS object between attempts. Do not send close_notify through a newly failed or unrelated socket.
- Bind session reuse to the intended endpoint and current verified configuration. Bound serialized session data and treat parse/restore failure as a full handshake, not plaintext.
- Check the pinned mbedTLS release against current upstream security advisories before a release decision.

## Preserve secrets and authentication

- Keep passwords, proxy credentials, SASL configs, SCRAM nonces, salts, proofs, server signatures, and queued sensitive sends out of diagnostics and ordinary logs.
- Use LockedSecret where the API promises locked storage. Fail closed when native page locking is required and unavailable.
- Wipe every owned copy on success, rejection, queue saturation, cancellation, allocation failure, exception, reconnect, and teardown. Include moved-from short-string storage and temporary encoded buffers.
- Validate SCRAM field counts, nonce binding, iteration bounds, salt sizes, GS2/authzid escaping, and server signatures before accepting success.
- Refuse credential authentication over plaintext. Treat EXTERNAL as opt-in only after a client certificate is provisioned.

## Preserve CTCP, flood, and DCC boundaries

- Parse CTCP framing and DCC numbers strictly. Suppress malformed or over-limit input without reflecting attacker content into logs.
- Apply monotonic flood admission before UI/model expansion, DCC setup, SOUND resolution, or other expensive work.
- Treat every DCC offer and inbound peer as hostile. Validate numeric address, scope, advertised port, expected peer, size, and all 64-bit offsets.
- Keep an accepted peer inert until the application explicitly accepts its token. Rejecting an unexpected peer must not consume the intended one-shot listener.
- Acknowledge received DCC bytes only after the application confirms its file write. Never let network progress outrun bounded uncommitted storage.
- Sanitize any eventual filename and confine writes to a caller-selected destination. The transfer engine must not decide filesystem trust.

## Verify the security properties

Run the focused test executables and then the full affected suite:

~~~sh
meson test -C <build-dir> comicchat-transport --print-errorlogs
meson test -C <build-dir> comicchat-dcc-transfer --print-errorlogs
meson test -C <build-dir> comicchat-ircv3 --print-errorlogs
~~~

Run ASan+UBSan for all parser, buffer, TLS, proxy, DCC, and secret-lifetime changes. Run TSan plus repeated stop/restart/cancel stress for worker, callback, queue, or generation changes. Keep frontend disabled in the TSan build.

Require tests for the affected failure paths: hostname mismatch, handshake deadline, cancellation, queue saturation, secret wiping, proxy malformation, reconnect, resumption, idle no-spin, DCC arithmetic, commit-gated ACKs, peer rejection, and restart. Add a new targeted test when the existing names do not prove the changed contract.

For adapter or build changes, also require the transport-retirement static gate, a focused adapter contract test, and a native Windows loopback. A successful MFC build, shared API compile, or window-launch smoke does not prove that a client retired its old socket path.

Report the red test, green commands, sanitizer results, fixed non-secret diagnostics, residual platform limitations, and any upstream advisory reviewed.
