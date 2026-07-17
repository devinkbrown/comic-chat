# Performance measurement plan

## Existing observability

| Surface | Metric or artifact | Repository proof |
| --- | --- | --- |
| Panel layout | panel-layout ns/op | portable/tests/bench.cpp |
| Render command assembly | pmr-render-batch-256 ns/op and checksum | portable/tests/bench.cpp |
| Deterministic dispatch | deterministic-task ns/op and checksum | portable/tests/bench.cpp |
| Idle network loop | delta of EngineStats.loop_iterations | portable/tests/transport_test.cpp |
| Command wakeups | EngineStats.command_wakeups | connection engine and tests |
| Queue pressure | queued bytes/commands, peak events, throttle deferrals | EngineStats and transport tests |
| Render fidelity | deterministic PNG, geometry, or hash | portable render tests and app --png |
| Memory limits | FrameArena capacity, primitive count, queue limits | memory and transport tests |

## Experiment record

Capture:

1. Full baseline and candidate commit hashes.
2. Compiler path/version, Meson version, buildtype, and relevant options.
3. CPU model, core policy, OS/kernel, available memory, and display backend.
4. Exact input fixture, asset, viewport, server transcript, or transfer size.
5. Warm-up, iterations, repetitions, and raw results.
6. Median, minimum, maximum, and a robust spread such as median absolute deviation.
7. Profile or counter evidence locating the cost.
8. Correctness artifact and all gates run after optimization.

## Profiling choices

- Use Linux perf for CPU samples and hardware counters when permitted.
- Use compiler optimization records to confirm inlining/vectorization questions instead of guessing.
- Use allocation counts or bounded arena/queue stats for memory claims.
- Use a trace or existing stats for wakeup and scheduling claims.
- Use native Windows profiling when a result depends on MFC, GDI, Win32 messaging, or MSVC code generation.

Do not add a permanent profiler dependency to the product merely to run a development experiment.

## Primary sources

- Linux perf tools: https://perf.wiki.kernel.org/index.php/Main_Page
- Clang optimization reports: https://clang.llvm.org/docs/UsersManual.html#options-to-emit-optimization-reports
- Meson test command: https://mesonbuild.com/Commands.html#test
- libuv metrics and loop behavior: https://docs.libuv.org/en/v1.x/loop.html
- SDL3 performance category: https://wiki.libsdl.org/SDL3/CategoryPerformance

Use these sources for tool semantics. Let the repository's benchmark and functional contracts define what Comic Chat must preserve.
