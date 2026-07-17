# Fuzzing contract

## Primary references

- LLVM libFuzzer: <https://llvm.org/docs/LibFuzzer.html>
- Clang SanitizerCoverage: <https://clang.llvm.org/docs/SanitizerCoverage.html>
- Clang AddressSanitizer: <https://clang.llvm.org/docs/AddressSanitizer.html>
- Clang UndefinedBehaviorSanitizer:
  <https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html>

LLVM describes libFuzzer as an in-process, coverage-guided engine driven by
SanitizerCoverage. Its target guidance is the repository baseline: tolerate all
inputs, remain deterministic and fast, avoid persistent global state, join
threads, and keep targets narrow. LLVM also notes that libFuzzer remains
supported for important bug fixes but is not receiving major feature work.
Keep Comic Chat's target body independent of the engine so a future runner can
change without rewriting the security contract.

## Initial target priority

1. IRC `LineFramer` and `Message::Parse`/checked serialization.
2. IRCv3 `Engine::Process` state transitions, CAP/SASL/batch/tag bounds, and
   recovery after rejection.
3. CTCP and SOUND path parsing, including embedded NUL, slash variants,
   traversal-like components, quoting, and maximum wire length.
4. Proxy response parsers and TLS session serialization boundaries without
   live sockets or secret material.
5. AVB, palette, mask, icon manifest, and history/import decoders that accept
   repository-controlled byte spans.

Prioritize a reachable pure boundary with a hard production limit. Do not begin
with the full SDL frontend, live libuv loop, MFC UI, or filesystem-driven
workflow when a smaller parser owns the actual trust boundary.

## Minimum target invariants

- No crash, leak, sanitizer report, unbounded allocation, hang, or thread left
  alive for any byte string within the declared maximum.
- A parser rejection does not mutate retained state or poison a later valid
  input.
- Successful checked serialization remains within the relevant wire/file
  limit and can be parsed back to the promised equivalence class.
- Any message rejected by the modern safety adapter never reaches the legacy
  Microsoft command handler.
- Secret-aware targets use synthetic bytes and wipe every intermediate on all
  exits; corpus and logs remain non-sensitive.
- Successful binary asset decodes respect dimensions, counts, offsets,
  multiplication bounds, palette indices, recursion/depth limits, and output
  allocation caps before rendering.

## Evidence tiers

- **Replay:** every committed seed runs once under a normal deterministic test.
- **Sanitized smoke:** ASan+UBSan target runs with explicit time, length, memory,
  and timeout caps on the committed corpus.
- **Campaign:** longer isolated execution records corpus/hash and coverage
  growth; it is diagnostic evidence, not a merge prerequisite unless the task
  contract says so.
- **Regression:** every confirmed defect has a non-fuzzer causal test that
  fails on the vulnerable baseline and passes on the repair.

Never translate "no findings in N runs" into a security guarantee. Report the
exact explored target and the important states the harness still cannot reach.
