# ComicChat model-scaled workflow

This workflow routes repository work across the current GPT-5.6 family while
keeping one accountable integrator. It is a policy and planning layer: the
calling agent or API runner remains responsible for starting workers, enforcing
path ownership, and collecting their results.

## Model lanes

| Lane | Default | Use for | Do not use alone for |
| --- | --- | --- | --- |
| Fast | `gpt-5.6-terra`, low | file inventory, searches, test-log classification, documentation checks, repetitive bounded edits | architectural decisions, security conclusions, release approval |
| Balanced | `gpt-5.6`, medium | focused implementation, tests, ordinary debugging, integration | final judgment on critical domains |
| Frontier | `gpt-5.6`, high/xhigh | decomposition, ambiguous failures, architecture, source-parity, security, release review, final synthesis | bulk work that a bounded lower lane can perform |

Model strings can be overridden with `COMICCHAT_MODEL_FAST`,
`COMICCHAT_MODEL_BALANCED`, and `COMICCHAT_MODEL_FRONTIER`. This lets the
workflow adopt a newly available model without rewriting task contracts.

Codex project agents are registered in `.codex/config.toml` and defined in
`.codex/agents/`. Use the narrowest matching specialist from the roster below;
use the general implementer only when no specialist owns the change.

| Phase | Agent |
| --- | --- |
| Intake and routing | `comicchat-orchestrator` |
| Read-only discovery | `comicchat-explorer` |
| Historical Microsoft evidence | `comicchat-source-historian` |
| Architecture blueprint | `comicchat-architect` |
| General Zig implementation | `comicchat-zig-implementer` |
| Comic layout/raster/assets | `comicchat-rendering-engineer` |
| IRC/IRCX/UDI/DCC/TLS | `comicchat-network-engineer` |
| Shared client/UI behavior | `comicchat-client-ui-engineer` |
| UI visual direction | `comicchat-ui-design-director` |
| UI primitives/tokens/states | `comicchat-ui-component-engineer` |
| UI interaction/focus/input | `comicchat-ui-interaction-engineer` |
| UI geometry/responsiveness | `comicchat-ui-layout-engineer` |
| Screenshot and state-matrix QA | `comicchat-visual-qa` |
| Labels/errors/empty-state copy | `comicchat-ui-copy-reviewer` |
| X11/Wayland | `comicchat-linux-platform-engineer` |
| Win32/Winsock | `comicchat-windows-platform-engineer` |
| Test and build verification | `comicchat-verifier` |
| Accessibility review | `comicchat-accessibility-reviewer` |
| Security review | `comicchat-security-reviewer` |
| Adversarial correctness review | `comicchat-adversarial-reviewer` |
| Docs, licensing, packaging | `comicchat-release-auditor` |
| Final integration | `comicchat-integrator` |

For heavy UI work, the minimum lane set is design director, component or
interaction implementer, visual QA, and accessibility review. Add layout,
platform, copy, source-history, or security lanes only when the affected
surface crosses those boundaries. The design director produces the brief; it
does not share implementation-file ownership.

## Route a task

Create a score from facts known before implementation:

- Scope: one file `0`, one subsystem `1`, cross-subsystem `2`, platform-wide `3`.
- Ambiguity: exact change `0`, some discovery `1`, unknown cause or design `2`.
- Risk: docs/tests only `0`, normal behavior `1`, user data/network/platform `2`, security/release/compatibility `3`.
- Verification: one focused gate `0`, unit plus build `1`, cross-platform/live/golden `2`.

Run:

```sh
python3 tools/ai_route.py --scope 2 --ambiguity 1 --risk 2 --verification 1 \
  --lanes 3 --domain protocol-compatibility
```

Scores `0-2` route to Fast, `3-6` to Balanced, and `7-10` to Frontier.
A critical domain always adds Frontier review even when the implementation is
safe to delegate to a lower lane. `--lanes` is the number of genuinely
independent workstreams, not a requested worker count; the router caps it at
the configured parallel limit.

## Execution graph

1. **Contract.** The integrator writes one task contract from
   `.ai/templates/task-contract.md`, including owned paths, forbidden paths,
   evidence, success criteria, and gates.
2. **Decompose.** Use Frontier when the task is ambiguous, cross-subsystem, or
   critical. Otherwise the integrator can decompose directly.
3. **Scout in parallel.** Give independent read-only questions to Fast workers.
   Each returns file-and-line evidence, uncertainties, and a recommended owned
   path. Do not ask several workers the same broad question.
4. **Implement in bounded lanes.** Use Balanced for normal code changes. Fast
   may perform mechanical edits only when the contract defines the exact
   transformation. One writer owns a file at a time.
5. **Verify cheaply and continuously.** Fast may classify logs and check docs;
   the writer runs focused tests. The integrator runs the authoritative gates.
6. **Escalate on evidence.** Escalate a lane one level for an unclear contract,
   repeated failure, cross-owner dependency, nondeterminism, or unexpected
   security/compatibility impact. Do not restart completed work when escalating;
   hand over the evidence bundle.
7. **Review by risk.** Critical domains require an independent Frontier review.
   Other Balanced changes receive at least a Fast diff-and-test review. The
   author may not be the only reviewer.
8. **Integrate once.** The integrator resolves conflicts, runs the gates in
   `docs/WORKERS.md`, checks `git diff --check`, and reports exact residual risk.

## Scaling rules

- Prefer one worker for a single bounded edit.
- Use two lanes when discovery and implementation can overlap without shared
  writes, or when Linux and Windows evidence can be gathered independently.
- Use three or four lanes only for separable subsystems, platform backends, or
  independent audit questions.
- Scale down when workers would touch the same files, depend on the same unknown,
  or require the same serial build resource.
- A higher score raises model capability; it does not automatically raise
  worker count.
- Frontier is the synthesizer for multi-lane work. Workers return evidence and
  patches; they do not independently redefine scope.

## ComicChat risk overrides

The following always require Frontier planning or review:

- TLS, certificate validation, SASL, session credentials, or proxy behavior.
- Thread/worker ownership, cancellation, reconnect, or event-loop changes.
- IRC/IRCX/UDI/DCC wire compatibility.
- Source-faithful comic layout, golden raster hashes, or asset provenance.
- Release packaging, licensing, or cross-platform claims.

Preserve the repository rules in `docs/WORKERS.md`: no SDL, no unpinned TLS
replacement, no insecure fallback, no unowned detached workers, and no golden
refresh without source evidence.

## Required handoff

Every lane returns:

1. task-contract ID and assigned model lane;
2. files read and changed;
3. concise findings with file-and-line evidence;
4. commands run and exact outcomes;
5. unresolved risks or dependencies;
6. whether the integrator should accept, escalate, or discard the result.

No lane may publish, deploy, release, merge, or make external writes unless the
task contract explicitly authorizes that act.
