---
name: comicchat-adversarial-testing
description: Design, implement, run, triage, and maintain deterministic fuzz targets and hostile-input corpora for Comic Chat parsers, protocol state machines, binary asset decoders, serializers, and security boundaries. Use when adding libFuzzer harnesses, replay tests, dictionaries, seed corpora, crash minimization, sanitizer-backed fuzzing, or regression promotion.
---

# Comic Chat adversarial testing

## Establish one attack surface

1. Read `AGENTS.md`, `docs/CPP26-ENGINEERING.md`, and
   `references/fuzzing-contract.md` before changing a harness or corpus.
2. Name one boundary and its production entry point: IRC framing/message
   parsing, IRCv3 state, CTCP/SOUND, proxy negotiation, TLS session data, AVB or
   asset decoding, icon metadata, history import, or another byte-facing API.
3. Trace the input through every parser, allocation, retained-state mutation,
   serializer, and legacy adapter that the target will exercise.
4. State the maximum accepted input, per-object and aggregate memory bounds,
   expected terminal outcomes, and invariants that must survive malformed data.
5. Add a deterministic replay test for known hostile examples before creating
   a coverage-guided target. A fuzzer supplements causal tests; it does not
   replace them.

Use `comicchat-ircv3-compat` for protocol semantics,
`comicchat-transport-security` for transport and secret boundaries, and
`comicchat-render-fidelity` for binary art whose successful decode has visual
invariants.

## Keep targets narrow and engine-independent

- Put reusable fuzz logic behind an ordinary byte-span function. Keep
  `LLVMFuzzerTestOneInput` a thin adapter so the same target can be replayed by
  a unit test or another fuzz engine.
- Prefer one parser or state transition family per target. Do not hide several
  unrelated slow subsystems behind a selector byte merely to reduce target
  count.
- Accept empty, arbitrary, malformed, and maximum-sized input without calling
  `exit`, aborting on an expected rejection, leaking a thread, or retaining
  cross-iteration state.
- Reset engines, clocks, random sources, queues, and registries for every
  iteration. Join all target-created threads before returning. Avoid network,
  filesystem, display-server, locale, wall-clock, and nondeterministic input.
- Reject input above the production boundary before copying or allocating it.
  The harness must not weaken limits to obtain more coverage.
- Assert semantic invariants as well as absence of crashes: checked
  serialization round-trips accepted values, rejected commands produce no
  legacy dispatch, bounded stores never exceed their caps, and later valid
  input remains usable after malformed input.
- Never print credentials or include real passwords, certificates, private
  keys, hostnames, or user logs in seeds or artifacts.

## Build with the matching Clang runtime

- Use the repository's supported Clang and its matching compiler-rt. Keep
  fuzz-only instrumentation out of ordinary binaries and native Windows MFC
  packages.
- Combine coverage-guided fuzzing with AddressSanitizer and
  UndefinedBehaviorSanitizer. Add other sanitizers only where the platform and
  target support them without masking the primary failure.
- Bound `max_len`, elapsed time, resident memory, and per-input timeout from the
  production contract. A timeout is a finding until complexity or sanitizer
  overhead is understood.
- Keep a fast deterministic corpus-replay test in normal CI. Put time-budgeted
  fuzz smoke and long-running campaigns in distinct jobs so an unavailable
  runner cannot silently erase regression coverage.
- Record exact compiler version, flags, target binary hash, corpus hash,
  dictionary, seed, duration, executions, coverage counters, peak memory, and
  sanitizer result. Iteration count alone is not a quality metric.

## Curate and minimize corpora

- Seed from small valid and invalid production-shaped examples, official
  protocol fixtures, boundary lengths, alternate encodings, and previous
  regressions. Use a grammar dictionary for meaningful IRC commands, tags,
  numerics, CTCP delimiters, AVB chunk identifiers, or other structured tokens.
- Keep corpus entries small, stable, uniquely named by content digest, and
  licensed for repository distribution. Do not commit a large generated
  corpus when a minimized subset preserves the same coverage.
- Minimize the corpus with the exact target and build configuration used to
  measure coverage. Review deletions so rare semantic states are not lost just
  because instrumentation changed.
- Treat crash, leak, out-of-memory, timeout, sanitizer report, assertion, and
  invariant violation as distinct outcomes. Preserve the original artifact
  privately until disclosure policy is decided.

## Promote every confirmed finding

1. Reproduce on the exact commit and sanitized target.
2. Minimize the input and determine whether the failure is in the harness or
   production code.
3. Add the smallest ordinary causal regression at the public boundary; use a
   binary corpus entry only when text or constructed bytes cannot express it.
4. Fix the production invariant, then replay the minimized input, full corpus,
   focused suite, and affected sanitizer suite.
5. Retain the minimized seed only when it adds coverage beyond the causal test.
6. Report fuzzing as bounded evidence, never as proof that the surface is
   vulnerability-free.

## Required handoff

Report the target and boundary, production limits, seed and dictionary sources,
baseline reproducer, exact instrumentation, corpus identity, measured campaign,
minimized findings, promoted regressions, and remaining unreachable states.
Never describe a campaign as executed unless its real output is attached.
