---
name: comicchat-supply-chain
description: Audit, select, update, pin, license, inventory, and attest Comic Chat's native dependencies and release inputs. Use when evaluating libraries, Meson wraps, submodules, vendored code, GitHub Actions, advisories, ABI/platform compatibility, SBOMs, provenance, artifact attestations, or dependency-related release risk.
---

# Comic Chat supply-chain security

## Inventory the artifact, not just the manifest

1. Read `AGENTS.md`, `docs/UNOFFICIAL-RELEASE.md`, and
   `references/dependency-contract.md` before changing a dependency or release
   input.
2. Name the exact shipped artifact and enumerate every direct and resolved
   native dependency, generator, compiler/runtime, action, submodule, wrap,
   vendored source tree, system fallback, and downloaded build input that can
   affect its bytes.
3. Record supplier, upstream repository, exact version or commit, immutable
   digest, acquisition path, license and notices, supported branch/EOL, known
   advisories, platform/ABI reach, and the repository files that pin it.
4. Prove which implementation is actually linked or packaged on Windows,
   Linux/Wayland, FreeBSD, and OpenBSD. A declared fallback is not a shipped
   dependency until build or artifact evidence reaches it.
5. Separate an inventory finding from an upgrade decision. Never rewrite a
   lock, wrap, submodule, action pin, or vendored tree during a read-only audit.

Use `comicchat-native-platforms` for ABI and OS integration,
`comicchat-transport-security` for mbedTLS/libuv behavior, and
`comicchat-verification-release` for exact-head packaging and publication.

## Select libraries by the product contract

- Prefer a maintained upstream with a documented security process, supported
  stable or LTS branch, responsive advisories, compatible license, native
  Clang/MSVC and target-OS support, stable C/C++ boundary, and reproducible
  source acquisition.
- Compare security maintenance, API/ABI fit, transitive complexity, binary
  size, runtime ownership, async model, portability, testability, package
  availability, and migration risk. Popularity or a single aggregate score is
  not a sufficient selection rule.
- Prefer the smallest dependency that owns a real product boundary. Do not add
  a framework that duplicates SDL3, Cairo, libuv, mbedTLS, ICU, or a standard
  facility without measured value and a retirement plan for the displaced
  code.
- Do not equate newest major with safest upgrade. For a security library,
  compare the current supported LTS patch to the new major's migration,
  platform, configuration, and audit cost before deciding.
- Require a removal/rollback path, owner, update cadence, advisory source, and
  acceptance tests for every new dependency.

## Pin and acquire inputs fail-closed

- Pin submodules and source wraps to immutable revisions and validate source
  archive digests. Pin third-party GitHub Actions by full commit SHA.
- Keep network downloads out of ordinary offline builds unless the existing
  wrap policy explicitly owns them. Never silently fall back from a verified
  source to an unpinned URL or different ABI.
- Preserve upstream signatures or attestations when available, but verify them
  against an explicit trusted identity and digest. A signature without a trust
  policy is metadata, not proof.
- Review generated code and binary blobs by provenance: generator version,
  source input, deterministic command, output digest, and license. Never hand
  edit a generated dependency output.
- Scope CI tokens and permissions to the minimum required operation. Keep
  untrusted pull-request code away from release credentials and writable
  provenance identities.

## Audit advisories and licenses

- Check the supplier's current security advisories and release notes, the
  relevant ecosystem advisory database, and repository security alerts. Record
  query time and exact version; advisory status is time-sensitive.
- Determine reachability from compiled features and shipped code before rating
  impact. "Dependency contains vulnerable code" and "artifact exposes the
  vulnerable path" are different claims; both need evidence.
- Treat unknown version, ambiguous fork provenance, EOL branch, missing source
  digest, incompatible license, missing notice, or unverifiable generated input
  as a release risk rather than assuming safety.
- For an urgent vulnerability, preserve a minimal backportable fix only when
  upstream identity and patch provenance are clear; otherwise update to a
  supported release and rerun the full ABI/security matrix.

## Produce verifiable release metadata

- Generate an SBOM from the resolved release build, not merely from declared
  Meson files. Include package identity, version, supplier, license, hashes,
  relationships, and the build/runtime components actually shipped.
- Tie each SBOM and checksum set to one artifact digest and exact source commit.
  Publish it beside the artifact or as an attached attestation, not as a stale
  hand-maintained repository list.
- Generate hosted build provenance for release artifacts and preserve the
  workflow identity, source revision, parameters, resolved dependencies,
  builder identity, and artifact digest. Verify the attestation against stated
  expectations before calling it evidence.
- Do not claim a SLSA level, reproducibility, hermeticity, or complete
  transitive inventory unless every requirement for that exact artifact has
  been verified. Provenance records how an artifact was produced; it does not
  by itself prove that dependencies are secure.

## Required handoff

Report the exact artifact and commit, dependency graph and acquisition paths,
pin/digest/license/advisory evidence, actual linkage and platform reachability,
proposed or applied changes, SBOM/provenance identity, verification commands,
and unresolved supply-chain risks. Cite primary supplier and specification
sources next to time-sensitive claims.
