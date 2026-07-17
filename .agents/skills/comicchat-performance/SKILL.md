---
name: comicchat-performance
description: Measure, diagnose, optimize, and regression-test Comic Chat's native C++ performance without weakening correctness, fidelity, bounds, or idle behavior. Use when investigating rendering, text, asset, scheduler, allocation, transport, startup, latency, throughput, memory, event-loop spin, or benchmark regressions under portable/ or modern Windows.
---

# Comic Chat performance

## Establish a measurable problem

1. Read AGENTS.md, portable/README.md, and references/measurement-plan.md.
2. Name the user-visible or resource outcome: frame time, startup, input latency, bytes per second, allocations, peak bounded memory, idle wakeups, or package size.
3. Reproduce it on an exact commit with a release build and stable input.
4. Capture correctness output and a baseline distribution before changing code.
5. Profile the real workload before selecting an optimization.

Do not optimize a synthetic loop that is unrelated to the reported path. Do not trade away source fidelity, protocol safety, deterministic behavior, or hard bounds for a faster number.

## Measure repeatably

- Record full git SHA, compiler and library versions, build options, CPU/OS, frontend/backend, input fixture, iteration count, and benchmark command.
- Compare the same machine and configuration. Do not compare debug, sanitizer, different compiler, different backend, or thermally unstable runs as if they were equivalent.
- Run enough repetitions to expose variance. Report median and spread, not only the best sample.
- Warm the path deliberately when measuring steady state; measure cold startup separately.
- Keep a consumed checksum or observable artifact so the optimizer cannot erase work.
- Use std::chrono::steady_clock for in-process elapsed time. Keep timing outside the operation being validated where practical.
- Store raw before/after output with the handoff. Treat ns/op as meaningful only with the exact workload and environment.

## Optimize within repository invariants

- Remove measured work, copies, allocations, lock contention, cache misses, or wakeups; do not start with broad micro-idiom churn.
- Preserve FrameArena's null upstream, RenderBatchBuilder limits, immutable generation snapshots, and explicit allocation failures.
- Preserve WorkerScheduler capacity, cancellation, and deterministic inline mode.
- Preserve ConnectionEngine and DCC queue limits, fairness, deadlines, secret wiping, generation rejection, and terminal-event reservations.
- Keep an idle connection asleep. Use EngineStats loop_iterations and command_wakeups to prove no-spin behavior.
- Keep rendering output and source-derived geometry byte- or pixel-equivalent unless the task explicitly changes the visual contract.
- Avoid adding persistent caches without a key, invalidation rule, memory bound, concurrency owner, and benchmark proving value.
- Avoid parallelism that makes goldens nondeterministic or creates more work than it removes.

## Extend benchmarks carefully

The existing portable/tests/bench.cpp covers panel layout, a 256-primitive PMR render batch, and deterministic task dispatch. Run it through:

~~~sh
meson test -C <build-dir> --suite perf --verbose
~~~

Add a microbenchmark only when it isolates the measured causal path. Give it fixed data, fixed iterations, a checksum, a stable name, and a timeout. Keep integration performance assertions, such as idle no-spin and bounded backpressure, in the functional test that owns the contract.

Do not put environment-sensitive absolute timing thresholds into general CI without a demonstrated stable runner distribution and an explicit tolerance policy.

## Prove no regression

1. Run the benchmark before and after on the same exact setup.
2. Run the focused correctness test and compare its semantic or visual artifact.
3. Run the full affected release suite.
4. Run ASan+UBSan for memory/ownership changes and TSan for concurrent optimizations.
5. Recheck Linux/Wayland, BSD, and Windows when the optimized path is shared.

Report raw results, median/spread, percentage and absolute change, profile evidence, correctness gates, memory-bound changes, and cases where the result is inconclusive.
