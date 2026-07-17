# Transport threat model

## Trust boundaries

| Boundary | Hostile behavior | Required result | Existing proof surface |
| --- | --- | --- | --- |
| DNS and addresses | Many, slow, mixed-family, stale results | Bound candidates and deadlines; cancel losing generations | portable/tests/transport_test.cpp |
| SOCKS5 / HTTP CONNECT | Oversized replies, bad methods/status, credential capture, ambiguous addresses | Bound parsing, validate protocol, keep IRC hostname verification | proxy loopback tests |
| TLS peer | Wrong hostname, untrusted chain, timeout, truncation, stale session | Fail closed; never downgrade; reset per attempt | mbedTLS loopback and hostname/deadline tests |
| Cross-thread API | Queue flood, stale generation, reentrant or throwing wakeup | Reject within bounds; preserve worker and terminal event | transport and DCC restart tests |
| IRC authentication | Credential replay/logging, hostile SCRAM fields, false terminal success | Secure transport only; validate proof/signature; wipe all copies | portable/tests/ircv3_test.cpp |
| IRC/CTCP input | Oversized frames, command injection, event amplification | Strict framing, bounded state, monotonic flood suppression | IRCv3 framing/flood tests |
| DCC peer | Forged address, unexpected connector, infinite file, ACK desync | Explicit peer acceptance, bounds, commit-gated ACK, prompt cancellation | portable/tests/dcc_transfer_test.cpp |
| UI adapter | Use-after-free, stale event, socket serialization from UI | Generation-tagged immutable events and typed commands | transport adapter/API tests |
| Legacy transport coexistence | MFC/direct socket remains reachable, TLS failure selects it, build omits shared engine | One `ConnectionEngine` IRC path per modern client; static ownership gate; no downgrade | transport-retirement gate, per-client adapter tests, native Windows loopback |

## Code map

- Shared connection API: portable/include/comicchat/net/connection_engine.hpp
- Shared connection implementation: portable/src/net/connection_engine.cpp
- DCC API and implementation: portable/include/comicchat/net/dcc_transfer_engine.hpp and portable/src/net/dcc_transfer_engine.cpp
- IRC/SASL/STS/flood policy: portable/include/comicchat/net/ircv3.hpp, portable/include/comicchat/net/flood.hpp, and portable/src/net/ircv3.cpp
- Secret storage and wiping: portable/include/comicchat/memory.hpp and portable/src/memory.cpp
- Crypto runtime policy: portable/include/comicchat/crypto_runtime.hpp, portable/include/comicchat/mbedtls_user_config.h, and portable/src/crypto_runtime.cpp
- Modern Windows v2.5 consumer: v2.5-beta-1-modern/transportuibridge.h, ircv3eventbridge.h, ircsock.cpp, ircproto.cpp, and filesend.cpp
- Retirement ledger and required v1/v2/portable integration gates: docs/TRANSPORT-RETIREMENT.md
- Pinned implementations: third_party/libuv and third_party/mbedtls

## Primary sources

- libuv design and loop ownership: https://docs.libuv.org/en/v1.x/design.html
- libuv async cross-thread wakeup: https://docs.libuv.org/en/v1.x/async.html
- mbedTLS hostname-verification guidance: https://mbed-tls.readthedocs.io/en/latest/kb/attacks/ssl_set_hostname/
- mbedTLS security advisories: https://mbed-tls.readthedocs.io/en/latest/security-advisories/
- TLS deployment recommendations, BCP 195: https://datatracker.ietf.org/doc/html/rfc9325
- SOCKS5: https://datatracker.ietf.org/doc/html/rfc1928
- SOCKS5 username/password authentication: https://datatracker.ietf.org/doc/html/rfc1929
- HTTP CONNECT semantics: https://www.rfc-editor.org/rfc/rfc9110.html#name-connect
- SASL: https://datatracker.ietf.org/doc/html/rfc4422
- SCRAM: https://datatracker.ietf.org/doc/html/rfc5802
- SCRAM-SHA-256: https://datatracker.ietf.org/doc/html/rfc7677
- IRCv3 SASL: https://ircv3.net/specs/extensions/sasl-3.2
- IRCv3 STS: https://ircv3.net/specs/extensions/sts
- Historical CTCP/DCC specification: https://www.irchelp.org/protocol/ctcpspec.html

DCC has no current IETF or IRCv3 standards authority. Use the historical CTCP/DCC document to understand wire compatibility, then let this repository's stricter bounds, explicit acceptance, and safe filesystem policy resolve ambiguity.
