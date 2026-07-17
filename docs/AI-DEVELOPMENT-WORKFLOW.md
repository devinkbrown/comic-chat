# Codex + Claude development workflow

This is the repository contract for using Codex and Claude together on
**Comic Chat: Reinked**. It turns the models into independent engineering roles;
compiler, test, sanitizer, specification, and runtime evidence remain the
authority.

The design follows the official guidance for [Codex subagents][codex-subagents],
[Codex best practices][codex-best], [Claude Code best practices][claude-best],
[Claude subagents][claude-subagents], [Claude model selection][claude-models],
and [Claude effort controls][claude-effort]. The important shared conclusion is
to protect the lead agent's context, give every worker a narrow job, isolate
writers, and require explicit verification.

## Authority and roles

Codex is the lead engineer and integrator. It owns requirements, decomposition,
the live integration branch, reconciliation of conflicting findings, final diff
review, release gates, GitHub state, and the final user report.

Claude is a second engineering perspective, not a second integrator. Use it for
independent primary-source research, a bounded draft in an isolated worktree,
or a fresh adversarial review. Claude must not merge or publish and must not be
the sole reviewer of a Claude-authored patch. A Claude-authored patch always
gets a fresh Codex review. A Codex-authored security, protocol, crypto, parser,
or concurrency patch always gets a Claude Opus/high adversarial review before
Codex's final integration decision.

Neither model can validate the other by agreement. A claim is accepted only
when it is tied to one or more of:

- a reproducer that fails on the baseline and passes on the patch;
- an official specification or the original Microsoft source/art;
- strict compiler, platform, sanitizer, or runtime evidence;
- a measured benchmark with a stable setup.

## Model and effort routing

Use the cheapest lane that preserves correctness. Claude effort is deliberately
capped at `high`; do not use `xhigh` or `max` in this repository.

| Work | Codex | Claude | Mode |
|---|---|---|---|
| Fast inventory, file ownership, log distillation | `gpt-5.6-terra`, low | Haiku, low | Read-only, parallel |
| Primary-source or protocol research | `gpt-5.6`, medium/high | Sonnet, medium | Read-only, independent citations |
| Bounded implementation | `gpt-5.6`, medium/high | Sonnet, medium | One writer in one isolated worktree |
| Architecture, concurrency, crypto, security | `gpt-5.6`, high | Opus, high | Independent threat/correctness passes |
| Final adversarial review | `gpt-5.6`, high | Opus, high | Fresh context, read-only |
| Integration and release | `gpt-5.6`, high when risky | None | Codex only |

Raise effort for uncertainty, state machines, ownership, concurrency, crypto,
parser ambiguity, or platform-specific behavior. Lower it for deterministic
inventory and log classification. Do not spend high effort on mechanical work
that a script or compiler answers exactly.

## End-to-end loop

### 1. Establish the oracle

Codex writes a compact task contract containing goal, relevant context,
constraints, and an observable definition of done. Rendering tasks name the
Microsoft source/art path. IRC tasks name the official specification and the
legacy parser/model consumer. Security fixes state the trust boundary and
failure policy.

### 2. Split independent questions

Run read-heavy discovery in parallel: source tracing, protocol research,
platform impact, test inventory, and threat review. Give each agent a bounded
question and required output. Do not ask several agents to broadly "review the
repo" or let multiple agents rediscover the same facts.

The lead thread retains decisions and summaries, not raw build logs. A worker
returns only confirmed findings, evidence, and unresolved uncertainty.

### 3. Prove the defect or contract first

For fixes, create a causal regression that fails on the unpatched baseline.
For features, create an executable contract or fixture before wiring the full
surface. Preserve the baseline exit code or assertion in the handoff.

Tests must exercise the real boundary. Parser-only tests do not prove legacy UI
adaptation; a mock socket does not prove TLS hostname verification; a generated
PNG does not prove source fidelity without a source-derived expectation.

### 4. Assign exactly one writer

Create a linked worktree and a topic branch for every parallel implementation.
Only its assigned writer edits it. Scopes may share read-only dependencies but
must not overlap files unless Codex deliberately serializes the work.

Claude drafts are uncommitted by default. Codex reviews the diff and either
revises it in that worktree or makes a clean, single-purpose commit before
integration. Never copy changes between trees manually when a reviewed commit
can be cherry-picked.

### 5. Cross-model adversarial review

Use a fresh reviewer context that did not author the patch. Review in this
priority order:

1. correctness and protocol/source fidelity;
2. security, secrets, bounds, and failure mode;
3. lifetime, cancellation, concurrency, and restart behavior;
4. legacy UI/model compatibility and platform parity;
5. missing causal tests and observability;
6. measured performance regressions;
7. maintainability only when it affects the above.

Reviewers report severity, file/line evidence, a concrete failure scenario,
and the smallest proving test. Read-only Claude reviewers do not get Bash and
must not claim they executed a command. Style-only opinions are not blockers.
Codex reproduces every release-blocking finding before changing code.

### 6. Run a verification ladder

Run gates from narrow to broad so failures stay attributable:

1. causal regression and changed target;
2. `git diff --check` and generated-asset verification;
3. strict C++26 release build and all affected tests;
4. ASan+UBSan for parser, ownership, transport, and memory changes;
5. TSan and restart/cancellation stress for concurrent code;
6. deterministic headless render plus visual evidence for UI/art changes;
7. Linux/Wayland, FreeBSD, OpenBSD, and Windows CI on the merged commit;
8. package/random-folder smoke tests for a release candidate.

Run the merged tree again. Passing in isolated topic branches does not prove the
integration result. Do not hide a timeout by extending it until the cause is
understood; when sanitizer overhead alone is proven, record the multiplier.

### 7. Integrate and release

Codex inspects commit ancestry and the complete diff, resolves overlap, and
cherry-picks one focused change at a time. After each risky change, rerun its
causal test. After the batch, run the complete release ladder.

Only Codex may push, update PR state, mark a PR ready, merge, tag, or publish.
Release requires all required CI jobs on the exact head commit, accurate
release metadata, and a clean worktree. "Locally green" is not release proof.

## Required handoff

Every worker ends with this compact block. Use `none` rather than omitting a
field, and include commands plus outcomes rather than "tests pass."

```text
HANDOFF
role: research | implementation | review | verification
status: complete | blocked
worktree: <absolute path or none>
base: <full commit>
head: <full commit or none>
commit: <full commit or none>
git_status: <porcelain-v1 output or clean>
diffstat: <git diff --stat summary or none>
patch_sha256: <scripts/ai/worktree-fingerprint.py value or none>
scope: <files/components inspected or changed>
oracle: <Microsoft source, official spec, or executable contract>
red: <baseline command, exit code, and exact failure, or none with reason>
green: <commands, exit codes, and exact pass counts/results>
sanitizers: <commands and results, or not-applicable with reason>
artifacts: <logs, images, traces, or none>
checks: <kind + exact tool + operation + exit code + result + exact evidence>
findings: <severity + file:line + failure scenario, or none>
risks: <remaining uncertainty or none>
next: <single recommended integration/review action>
END HANDOFF
```

For an uncommitted implementation, `patch_sha256` identifies the exact draft,
including sorted untracked paths and bytes. Dirty initialized submodules are
rejected because a superproject diff cannot identify their internal bytes.
Codex computes the fingerprint with `scripts/ai/worktree-fingerprint.py`, then
freezes that worktree: no writer may touch it until review finishes. The
reviewer recomputes the value before and after review. Any mismatch invalidates
the review and all prior evidence.

The current native Windows CI compiles, packages, and launches the MFC clients
from random folders. Do not claim that a Windows test source is gated merely
because it exists: cite the workflow command that builds/runs it, or record it
as a coverage gap and add the gate.

## Repository tooling

Project-specific agents live in `.codex/agents/` and `.claude/agents/`. The
Claude command wrapper provides consistent non-interactive lanes:

```sh
scripts/ai/claude-consult.sh inventory prompt.txt
scripts/ai/claude-consult.sh research prompt.txt
scripts/ai/claude-consult.sh draft prompt.txt       # linked worktree only
scripts/ai/claude-consult.sh review prompt.txt
scripts/ai/claude-consult.sh security prompt.txt
```

The wrapper uses Claude safe mode to exclude auto-memory and unrelated local
customizations, injects this repository contract explicitly, disables session
persistence, caps effort, removes Bash from every lane, and refuses a draft in
the primary worktree. Project settings and a deterministic hook also deny Bash,
so Claude proposes commands and Codex executes them. Project worktrees branch
from the current local `HEAD`, so unpushed integration commits are present.

Output is validated locally against `scripts/ai/claude-handoff.schema.json`,
including the outer CLI success envelope and check/result consistency. The
wrapper replaces model-supplied role and Git fields with locally measured
metadata, fingerprints the worktree, and invalidates read-only evidence if the
tree changes during the consultation. A blocked handoff or reported failed
check makes the wrapper fail. This does not replace Codex's scope assignment or
review.

Claude checks distinguish `model-tool` observations from `proposed-command`
verification. A model-tool record names the exact available Read/Grep/Glob/Web
or edit tool and may report its result. Every shell, compiler, test, sanitizer,
or benchmark command is only a proposed command with `result: not-run` and
`exit_code: -1`; `red`, `green`, and `sanitizers` likewise remain `not-run` or
`not-applicable` as exact tokens without appended prose. Explanations belong in
`risks` or proposed-command checks. Only Codex or CI execution may promote those commands to
verification evidence.

Hooks are intentionally reserved for cheap deterministic invariants. Long test
suites belong in explicit verification and CI, where failures are visible and
reproducible; do not attach a full build to every model stop event.

[codex-subagents]: https://learn.chatgpt.com/docs/agent-configuration/subagents
[codex-best]: https://learn.chatgpt.com/guides/best-practices
[claude-best]: https://code.claude.com/docs/en/best-practices
[claude-subagents]: https://code.claude.com/docs/en/sub-agents
[claude-models]: https://code.claude.com/docs/en/model-config
[claude-effort]: https://platform.claude.com/docs/en/build-with-claude/effort
