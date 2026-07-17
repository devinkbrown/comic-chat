#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

usage() {
  cat <<'EOF'
Usage: scripts/ai/claude-consult.sh <lane> <prompt-file|->

Available lanes:
EOF
  python3 "$script_dir/agent-route.py" --list 2>/dev/null | sed 's/^/  /'
  cat <<'EOF'

Set COMICCHAT_CLAUDE_DRY_RUN=1 to print the configured invocation without
starting Claude. Writer lanes are forbidden in the primary worktree.
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
repo_root=$(realpath -- "$repo_root") || {
  printf 'Cannot resolve the current git worktree\n' >&2
  exit 69
}
wrapper_root=$(realpath -- "$script_dir/../..") || {
  printf 'Cannot resolve the Claude wrapper repository\n' >&2
  exit 69
}
if [[ "$repo_root" != "$wrapper_root" ||
      ! -f "$repo_root/portable/meson.build" ||
      ! -f "$repo_root/v2.5-beta-1-modern/chat.mak" ]]; then
  printf 'Refusing to run outside the Comic Chat legacy-fork worktree\n' >&2
  exit 69
fi
cd "$repo_root"

for required_path in \
  AGENTS.md \
  docs/AI-DEVELOPMENT-WORKFLOW.md \
  docs/CPP26-ENGINEERING.md \
  scripts/ai/agent-roster.json \
  scripts/ai/agent-route.py \
  scripts/ai/claude-consult.sh \
  scripts/ai/claude-model-handoff.schema.json \
  scripts/ai/claude-handoff.schema.json \
  scripts/ai/validate-claude-handoff.py \
  scripts/ai/worktree-fingerprint.py; do
  if [[ ! -r "$required_path" ]]; then
    printf 'Missing or unreadable Claude workflow input: %s\n' "$required_path" >&2
    exit 66
  fi
done

temp_root=${COMICCHAT_AI_TMPDIR:-${TMPDIR:-/var/tmp}}
temp_root=$(realpath -- "$temp_root" 2>/dev/null || true)
if [[ -z "$temp_root" || ! -d "$temp_root" || ! -w "$temp_root" ||
      "$temp_root" == "$repo_root" || "$temp_root" == "$repo_root"/* ]]; then
  printf 'Claude temporary storage must be a writable directory outside the repository.\n' >&2
  exit 73
fi

pre_contract_fingerprint=
if [[ "${COMICCHAT_CLAUDE_DRY_RUN:-0}" != "1" ]]; then
  pre_contract_fingerprint=$(python3 -I scripts/ai/worktree-fingerprint.py --repo "$repo_root")
fi

route_line=$(python3 "$script_dir/agent-route.py" "$lane") || {
  usage >&2
  exit 64
}
IFS=$'\t' read -r agent_name model effort permission tools denied_tools \
  max_turns writer_lane handoff_role skills lane_contract <<<"$route_line"

skill_contract=
control_files=(
  AGENTS.md
  CLAUDE.md
  docs/AI-DEVELOPMENT-WORKFLOW.md
  docs/CPP26-ENGINEERING.md
  .claude/settings.json
  ".claude/agents/${agent_name}.md"
  scripts/ai/agent-roster.json
  scripts/ai/agent-route.py
  scripts/ai/claude-consult.sh
  scripts/ai/claude-model-handoff.schema.json
  scripts/ai/claude-handoff.schema.json
  scripts/ai/validate-claude-handoff.py
  scripts/ai/worktree-fingerprint.py
)
IFS=',' read -r -a routed_skills <<<"$skills"
for skill in "${routed_skills[@]}"; do
  canonical_skill=".agents/skills/$skill/SKILL.md"
  claude_skill=".claude/skills/$skill/SKILL.md"
  if [[ ! -r "$canonical_skill" || ! -r "$claude_skill" ]]; then
    printf 'Missing routed project skill: %s\n' "$skill" >&2
    exit 66
  fi
  skill_contract+=$'\n\n===== BEGIN ROUTED PROJECT SKILL: '
  skill_contract+="$skill"
  skill_contract+=$' =====\n'
  skill_contract+=$(<"$canonical_skill")
  skill_contract+=$'\n===== END ROUTED PROJECT SKILL ====='
  control_files+=("$canonical_skill" "$claude_skill")
done

for control_file in "${control_files[@]}"; do
  if [[ ! -r "$control_file" ]]; then
    printf 'Missing or unreadable Claude control-plane input: %s\n' "$control_file" >&2
    exit 66
  fi
done

control_digest() {
  python3 -I - "${control_files[@]}" <<'PY'
import hashlib
import sys
from pathlib import Path

digest = hashlib.sha256()
for raw_path in sys.argv[1:]:
    path = Path(raw_path)
    payload = path.read_bytes()
    encoded = raw_path.encode("utf-8")
    digest.update(len(encoded).to_bytes(8, "big"))
    digest.update(encoded)
    digest.update(len(payload).to_bytes(8, "big"))
    digest.update(payload)
print(digest.hexdigest())
PY
}

snapshot_digest() {
  python3 -I - \
    "$control_dir/worktree-fingerprint.py" \
    "$control_dir/validate-claude-handoff.py" \
    "$control_dir/model-handoff.schema.json" \
    "$control_dir/final-handoff.schema.json" <<'PY'
import hashlib
import sys
from pathlib import Path

digest = hashlib.sha256()
for raw_path in sys.argv[1:]:
    payload = Path(raw_path).read_bytes()
    digest.update(len(payload).to_bytes(8, "big"))
    digest.update(payload)
print(digest.hexdigest())
PY
}

if [[ "$writer_lane" == "1" ]]; then
  git_dir=$(git rev-parse --path-format=absolute --git-dir)
  common_dir=$(git rev-parse --path-format=absolute --git-common-dir)
  if [[ "$git_dir" == "$common_dir" &&
        !( "${COMICCHAT_CLAUDE_DRY_RUN:-0}" == "1" &&
           "${COMICCHAT_CLAUDE_VALIDATE_WRITER:-0}" == "1" ) ]]; then
    printf 'Refusing Claude writer lane in the primary worktree; create a linked worktree first.\n' >&2
    exit 77
  fi
fi

model_handoff_schema_path=scripts/ai/claude-model-handoff.schema.json
final_handoff_schema_path=scripts/ai/claude-handoff.schema.json
model_handoff_schema=$(<"$model_handoff_schema_path")

common_contract=$(cat AGENTS.md docs/AI-DEVELOPMENT-WORKFLOW.md; cat <<EOF

CLAUDE LANE OVERRIDE
${lane_contract}

Load and follow these project skills before doing the assigned work:
${skills}
${skill_contract}

Claude never merges, rebases, pushes, publishes, changes PR state, or supplies
the only review of its own work. Compiler, test, sanitizer, official spec, and
Microsoft source evidence outrank model agreement. Return only the structured
handoff requested by the supplied JSON Schema. Use result "not-run" and exit
code -1 and kind "proposed-command" for every shell/build/test command, because
this lane has no Bash. Executed Read/Grep/Glob/WebFetch/WebSearch/Edit/Write
tools use kind "model-tool", their exact tool name, and a matching command
prefix. StructuredOutput is envelope machinery and must not appear in checks.
Do not add role, red, green, sanitizers, worktree, base, head, commit,
git_status, diffstat, patch_sha256, or model_report_trust: they are intentionally
absent from the model schema and the trusted wrapper supplies them. Put explanations in risks
or proposed-command checks. Never invent git status, commit, test, sanitizer,
benchmark, or runtime evidence.
EOF
)

args=(
  claude
  --print
  --setting-sources project
  --agent "$agent_name"
  --no-session-persistence
  --strict-mcp-config
  --no-chrome
  --output-format json
  --json-schema "$model_handoff_schema"
  --name "comicchat-${lane}"
  --model "$model"
  --effort "$effort"
  --max-turns "$max_turns"
  --permission-mode "$permission"
  --tools "$tools"
  --disallowed-tools "$denied_tools"
  --append-system-prompt "$common_contract"
)

if [[ "${COMICCHAT_CLAUDE_DRY_RUN:-0}" == "1" ]]; then
  printf '%q ' "${args[@]}"
  printf '\n'
  exit 0
fi

control_dir=$(mktemp -d "$temp_root/comicchat-claude-control.XXXXXX")
output_file=
cleanup() {
  if [[ -n "$output_file" && "${COMICCHAT_CLAUDE_KEEP_OUTPUT:-0}" == "1" ]]; then
    printf 'Preserved raw Claude envelope: %s\n' "$output_file" >&2
  elif [[ -n "$output_file" ]]; then
    rm -f "$output_file"
  fi
  chmod 0700 "$control_dir"
  rm -f \
    "$control_dir/worktree-fingerprint.py" \
    "$control_dir/validate-claude-handoff.py" \
    "$control_dir/model-handoff.schema.json" \
    "$control_dir/final-handoff.schema.json"
  rmdir "$control_dir"
}
trap cleanup EXIT

install -m 0400 scripts/ai/worktree-fingerprint.py "$control_dir/worktree-fingerprint.py"
install -m 0400 scripts/ai/validate-claude-handoff.py "$control_dir/validate-claude-handoff.py"
install -m 0400 "$model_handoff_schema_path" "$control_dir/model-handoff.schema.json"
install -m 0400 "$final_handoff_schema_path" "$control_dir/final-handoff.schema.json"
snapshot_digest_before=$(snapshot_digest)
chmod 0500 "$control_dir"
control_digest_before=$(control_digest)
initial_head=$(git rev-parse HEAD)
initial_fingerprint=$(python3 -I "$control_dir/worktree-fingerprint.py" --repo "$repo_root")
if [[ "$pre_contract_fingerprint" != "$initial_fingerprint" ]]; then
  printf 'Worktree changed while the Claude contract was being captured.\n' >&2
  exit 75
fi

command -v claude >/dev/null 2>&1 || {
  printf 'claude is required\n' >&2
  exit 69
}
claude_version=$(claude --version | awk 'NR == 1 { print $1 }')
python3 "$script_dir/agent-route.py" --check-cli-version "$claude_version" || {
  printf 'Install a supported Claude Code CLI before running a consultation.\n' >&2
  exit 69
}

output_file=$(mktemp "$temp_root/comicchat-claude-handoff.XXXXXX.json")

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

control_digest_after=$(control_digest)
if [[ "$control_digest_before" != "$control_digest_after" ]]; then
  printf 'Claude changed its own control plane; rejecting the handoff.\n' >&2
  exit 75
fi
snapshot_digest_after=$(snapshot_digest)
if [[ "$snapshot_digest_before" != "$snapshot_digest_after" ]]; then
  printf 'Claude changed the immutable validation snapshot; rejecting the handoff.\n' >&2
  exit 75
fi
final_fingerprint=$(python3 -I "$control_dir/worktree-fingerprint.py" --repo "$repo_root")
if [[ "$writer_lane" == "0" ]]; then
  if [[ "$initial_fingerprint" != "$final_fingerprint" ]]; then
    printf 'Read-only Claude lane observed a concurrent worktree change; evidence is invalid.\n' >&2
    exit 75
  fi
fi

python3 -I "$control_dir/validate-claude-handoff.py" \
  "$output_file" "$control_dir/model-handoff.schema.json" \
  "$control_dir/final-handoff.schema.json" \
  --repo "$repo_root" --base "$initial_head" --role "$handoff_role" \
  --expected-model "$model" --allowed-tools "$tools" \
  --expected-fingerprint "$final_fingerprint" \
  --fingerprint-script "$control_dir/worktree-fingerprint.py"
