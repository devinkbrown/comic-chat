# ComicChat agent entrypoint

Read `docs/WORKERS.md` before changing this repository. Its source-fidelity,
platform, transport, asset, and verification rules are mandatory.

For non-trivial work, follow `.ai/WORKFLOW.md`:

1. Score the task with `python3 tools/ai_route.py`.
2. Record scope, owned paths, acceptance criteria, and gates using
   `.ai/templates/task-contract.md` before delegating work.
3. Use only as many parallel lanes as can work independently. One writer owns a
   file at a time; read-only scouts must return file-and-line evidence.
4. Escalate unclear, repeatedly failing, cross-owner, nondeterministic, or newly
   critical work with its existing evidence instead of restarting it.
5. Require independent Frontier review for every critical domain identified by
   `.ai/model-routing.json`.
6. Keep one integrator accountable for scope, authoritative verification, and
   the final result.

Do not let a routing recommendation override user scope, repository invariants,
or authorization boundaries. Never publish, deploy, release, merge, or make
other external writes unless the user explicitly authorizes that action.
