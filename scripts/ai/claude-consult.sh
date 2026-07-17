#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: scripts/ai/claude-consult.sh <lane> <prompt-file|->

Lanes:
  inventory  Haiku / low / read-only
  research   Sonnet / medium / read-only
  draft      Sonnet / medium / write-enabled linked worktree only
  review     Opus / high / read-only
  security   Opus / high / read-only

Set COMICCHAT_CLAUDE_DRY_RUN=1 to print the configured invocation without
starting Claude. Drafts are forbidden in the primary worktree.
EOF
}

if [[ $# -ne 2 ]]; then
  usage >&2
  exit 64
fi

lane=$1
prompt_file=$2

command -v git >/dev/null 2>&1 || {
  printf 'git is required\n' >&2
  exit 69
}

if [[ "$prompt_file" != "-" ]]; then
  if [[ "$prompt_file" != /* ]]; then
    prompt_file="$PWD/$prompt_file"
  fi
  prompt_file=$(realpath -- "$prompt_file" 2>/dev/null || true)
  if [[ -z "$prompt_file" || ! -f "$prompt_file" ]]; then
    printf 'Prompt file does not exist\n' >&2
    exit 66
  fi
fi

repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || {
  printf 'Run this command inside a Comic Chat git worktree\n' >&2
  exit 69
}
cd "$repo_root"

permission=plan
tools=Read,Grep,Glob
lane_contract=
max_turns=40

case "$lane" in
  inventory)
    handoff_role=research
    model=haiku
    effort=low
    max_turns=20
    lane_contract='Map only the assigned files, ownership, tests, and platform impact. Do not run commands or propose broad redesigns.'
    ;;
  research)
    handoff_role=research
    model=sonnet
    effort=medium
    tools=Read,Grep,Glob,WebFetch,WebSearch
    lane_contract='Research independently from primary sources. Do not run commands. Separate confirmed facts from inference and cite exact sources.'
    ;;
  draft)
    handoff_role=implementation
    model=sonnet
    effort=medium
    permission=acceptEdits
    tools=Read,Grep,Glob,Edit,Write
    max_turns=60
    lane_contract='Draft only the assigned change. You cannot run commands: propose exact causal and verification commands for Codex. Leave an uncommitted diff for review.'

    git_dir=$(git rev-parse --path-format=absolute --git-dir)
    common_dir=$(git rev-parse --path-format=absolute --git-common-dir)
    if [[ "$git_dir" == "$common_dir" &&
          !( "${COMICCHAT_CLAUDE_DRY_RUN:-0}" == "1" &&
             "${COMICCHAT_CLAUDE_VALIDATE_DRAFT:-0}" == "1" ) ]]; then
      printf 'Refusing Claude draft in the primary worktree; create a linked worktree first.\n' >&2
      exit 77
    fi
    ;;
  review)
    handoff_role=review
    model=opus
    effort=high
    max_turns=50
    lane_contract='Act as a fresh adversarial reviewer. Do not run commands. Confirm source defects with file:line evidence, propose causal tests for Codex, and ignore style-only opinions.'
    ;;
  security)
    handoff_role=review
    model=opus
    effort=high
    tools=Read,Grep,Glob,WebFetch,WebSearch
    max_turns=60
    lane_contract='Audit trust boundaries, secrets, bounds, downgrade behavior, lifetime, cancellation, and concurrency. Do not run commands. Report only source-evidenced findings and give Codex exact reproductions.'
    ;;
  *)
    printf 'Unknown lane: %s\n' "$lane" >&2
    usage >&2
    exit 64
    ;;
esac

handoff_schema_path=scripts/ai/claude-handoff.schema.json
if [[ ! -f "$handoff_schema_path" ]]; then
  printf 'Missing Claude handoff schema: %s\n' "$handoff_schema_path" >&2
  exit 66
fi
handoff_schema=$(<"$handoff_schema_path")

common_contract=$(cat AGENTS.md docs/AI-DEVELOPMENT-WORKFLOW.md; cat <<EOF

CLAUDE LANE OVERRIDE
${lane_contract}

Claude never merges, rebases, pushes, publishes, changes PR state, or supplies
the only review of its own work. Compiler, test, sanitizer, official spec, and
Microsoft source evidence outrank model agreement. Return only the structured
handoff requested by the supplied JSON Schema. Use result "not-run" and exit
code -1 and kind "proposed-command" for every shell/build/test command, because
this lane has no Bash. Executed Read/Grep/Glob/Web/Edit tools use kind
"model-tool", their exact tool name, and a matching command prefix. Set red,
green, and sanitizers to the exact token "not-run" or "not-applicable" with no
added prose; put explanations in risks or proposed-command checks. Never invent git status,
commit, test, sanitizer, benchmark, or runtime evidence. Set worktree, base, head, commit,
git_status, diffstat, and patch_sha256 to "not-run"; the trusted wrapper will
replace them with locally measured values after you return.
EOF
)

args=(
  claude
  --print
  --safe-mode
  --no-session-persistence
  --strict-mcp-config
  --output-format json
  --json-schema "$handoff_schema"
  --name "comicchat-${lane}"
  --model "$model"
  --effort "$effort"
  --max-turns "$max_turns"
  --permission-mode "$permission"
  --tools "$tools"
  --append-system-prompt "$common_contract"
)

if [[ "${COMICCHAT_CLAUDE_DRY_RUN:-0}" == "1" ]]; then
  printf '%q ' "${args[@]}"
  printf '\n'
  exit 0
fi

initial_head=$(git rev-parse HEAD)
initial_fingerprint=$(python3 scripts/ai/worktree-fingerprint.py --repo "$repo_root")

command -v claude >/dev/null 2>&1 || {
  printf 'claude is required\n' >&2
  exit 69
}

output_file=$(mktemp "${COMICCHAT_AI_TMPDIR:-${TMPDIR:-/var/tmp}}/comicchat-claude-handoff.XXXXXX.json")
trap 'rm -f "$output_file"' EXIT

set +e
if [[ "$prompt_file" == "-" ]]; then
  "${args[@]}" >"$output_file"
else
  "${args[@]}" <"$prompt_file" >"$output_file"
fi
claude_status=$?
set -e

if [[ $claude_status -ne 0 ]]; then
  printf 'Claude lane failed with exit %d\n' "$claude_status" >&2
  cat "$output_file" >&2
  exit "$claude_status"
fi

if [[ "$lane" != "draft" ]]; then
  final_fingerprint=$(python3 scripts/ai/worktree-fingerprint.py --repo "$repo_root")
  if [[ "$initial_fingerprint" != "$final_fingerprint" ]]; then
    printf 'Read-only Claude lane observed a concurrent worktree change; evidence is invalid.\n' >&2
    exit 75
  fi
fi

python3 scripts/ai/validate-claude-handoff.py \
  "$output_file" "$handoff_schema_path" \
  --repo "$repo_root" --base "$initial_head" --role "$handoff_role" \
  --allowed-tools "$tools"
