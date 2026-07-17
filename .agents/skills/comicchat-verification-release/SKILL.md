---
name: comicchat-verification-release
description: Verify Comic Chat changes and, only when explicitly authorized, prepare or publish an unofficial release from an exact commit. Use when running final gates, validating CI or sanitizer evidence, checking submodule and generated-asset pins, packaging Windows builds, smoke-testing archives, auditing checksums/provenance, tagging, or deciding release readiness.
---

# Comic Chat verification and release

## Freeze the candidate

1. Read AGENTS.md, docs/AI-DEVELOPMENT-WORKFLOW.md, docs/UNOFFICIAL-RELEASE.md, and references/release-gates.md.
2. Record the full candidate commit and expected release scope.
3. Require a clean worktree and initialized submodules at their recorded commits.
4. Stop mutation while collecting evidence. Any new commit invalidates exact-head CI and artifact claims.
5. Distinguish verification authority from publication authority. Do not push, tag, merge, change PR state, or publish unless the user explicitly requests it.

## Run the local ladder

Run narrow causal tests first, then generated-asset checks, strict release build, full affected tests, sanitizer lanes, headless render, and platform smokes. Use the exact commands in references/release-gates.md and the current workflows.

- Treat warnings under werror, skipped tests, timeouts, sanitizer findings, missing tools, dirty submodules, or stale generated assets as failures or missing evidence.
- Re-run integrated code. Green topic branches do not prove the merged candidate.
- Reproduce every release-blocking model or review finding with a test, compiler, sanitizer, specification, or source oracle.
- Check the current mbedTLS and other security advisories when network or crypto code ships.

## Require exact-head CI

- Resolve the full candidate SHA locally and remotely.
- Require portable strict C++26, real Wayland, FreeBSD, OpenBSD, native Windows build/package/smoke, and any affected workflow-specific jobs on that exact SHA.
- Match each run's head SHA, workflow revision, conclusion, and required job set. A green branch, older run, rerun on another commit, or partial matrix is not release proof.
- Inspect failed or cancelled job logs. Do not convert timeout or unavailable infrastructure into a pass.
- Pin external GitHub Actions by immutable commit as the existing workflows do.

## Verify release artifacts

- Build packages in CI or a clean trusted Windows environment from the frozen SHA.
- Preserve the two unofficial modern ZIPs and SHA256SUMS.txt contract unless the release scope explicitly changes it.
- Extract each archive into a random path containing spaces and launch it with an unrelated working directory.
- Verify the expected executable, ComicArt/resources, help, license, notices, and provenance metadata.
- Recompute SHA-256 after download and compare it with SHA256SUMS.txt.
- Confirm artifacts and names identify the source commit and remain unsigned, unsupported, unofficial archival builds.
- Never add signing secrets, installers, credentials, local caches, build directories, or unrelated artifacts to the archive.

## Publish only after authorization

When publication is explicitly requested:

1. Confirm the tag target equals the verified full SHA.
2. Use an accurate date-based unofficial tag and mark the GitHub release as a pre-release.
3. Attach only the verified ZIPs and checksum file.
4. State the exact source commit, unsigned status, unofficial/unsupported status, and extraction requirement.
5. Re-fetch the published release, verify tag ancestry and checksums, and report public evidence.

If any required gate is absent, report not ready with the failing command/job and the smallest next action. Do not soften the release condition.
