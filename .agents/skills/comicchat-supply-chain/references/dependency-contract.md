# Dependency and provenance contract

## Primary standards and services

- SLSA 1.2: <https://slsa.dev/spec/v1.2/>
- SLSA build provenance: <https://slsa.dev/spec/v1.2/build-provenance>
- SLSA artifact verification:
  <https://slsa.dev/spec/v1.2/verifying-artifacts>
- SPDX specifications: <https://spdx.dev/use/specifications/>
- GitHub artifact attestations:
  <https://docs.github.com/en/actions/concepts/security/artifact-attestations>
- GitHub dependency review:
  <https://docs.github.com/en/code-security/supply-chain-security/understanding-your-software-supply-chain/about-dependency-review>
- OpenSSF Scorecard checks: <https://github.com/ossf/scorecard/blob/main/docs/checks.md>

SLSA provenance is verifiable information about where, when, and how an
artifact was produced. Verification still has to authenticate the envelope and
compare builder identity, build type, parameters, source, and artifact digest
against explicit expectations. An SBOM describes components and relationships;
it is complementary to provenance, not interchangeable with it. Scorecard
results are heuristics and can guide inspection, but are neither vulnerability
proof nor an automatic dependency acceptance threshold.

## Repository dependency families

Audit at least these families when they can affect the requested artifact:

- mbedTLS and PSA crypto configuration, release branch, advisories, trust-store
  integration, and compiled feature set;
- libuv event-loop/DNS/thread behavior and native backend support;
- SDL3, Cairo, pixman, FreeType, HarfBuzz, ICU, and image/font dependencies in
  the portable renderer and package;
- Meson/Ninja, Clang/compiler-rt/libc++ or platform C++ runtime, and MSVC/MFC
  toolchain inputs;
- wraps, submodules, vendored source archives, generated MIDL/resource/icon
  outputs, and system-library fallback rules;
- every GitHub Action and packaging/provenance helper used by the release job.

For each platform, distinguish build-only, static runtime, dynamic runtime,
optional, test-only, and unused declarations. Confirm with link maps, dependency
inspection, or packaged-file evidence on the exact release candidate.

## Upgrade acceptance

An upgrade needs all of the following before integration:

1. exact old/new versions, source identities, checksums, and upstream release
   or advisory rationale;
2. API, ABI, configuration-default, license, toolchain, and OS impact review;
3. feature-equivalent secure configuration, especially TLS verification,
   protocol minimums, RNG, threading, allocator, and cleanup behavior;
4. strict build, affected unit/integration tests, sanitizers, native platform
   matrix, package smoke, and artifact linkage evidence;
5. updated notices, lock/wrap/submodule pins, SBOM inputs, and rollback notes.

Do not combine several major dependency migrations into one unreviewable patch.
Land the smallest independently verifiable update and rerun the integrated
matrix before the next one.
