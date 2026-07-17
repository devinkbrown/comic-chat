#!/usr/bin/env python3
"""Dependency-free validation for the repository's Codex + Claude contract."""

from __future__ import annotations

import json
import os
import re
import stat
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CLAUDE_AGENTS = {
    "fast-inventory": ("haiku", "low", "plan", False),
    "protocol-researcher": ("sonnet", "medium", "plan", False),
    "implementation-drafter": ("sonnet", "medium", "acceptEdits", True),
    "security-reviewer": ("opus", "high", "plan", False),
    "verification-reviewer": ("opus", "high", "plan", False),
}
CODEX_AGENTS = {
    "comicchat-fast-inventory": ("gpt-5.6-terra", "low", "read-only"),
    "comicchat-protocol-reviewer": ("gpt-5.6", "high", "read-only"),
    "comicchat-security-reviewer": ("gpt-5.6", "high", "read-only"),
    "comicchat-integration-worker": ("gpt-5.6", "medium", "workspace-write"),
}


def temporary_root() -> str | None:
    candidates = (
        os.environ.get("COMICCHAT_AI_TMPDIR"),
        os.environ.get("TMPDIR"),
        "/var/tmp",
    )
    for candidate in candidates:
        if candidate and Path(candidate).is_dir() and os.access(candidate, os.W_OK | os.X_OK):
            return candidate
    return None


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(
    command: list[str],
    *,
    cwd: Path = ROOT,
    env: dict[str, str] | None = None,
    input_text: str | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        input=input_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def parse_frontmatter(path: Path) -> dict[str, object]:
    content = path.read_text()
    require(content.startswith("---\n"), f"{path}: missing YAML frontmatter")
    try:
        raw, body = content[4:].split("\n---\n", 1)
    except ValueError as error:
        raise RuntimeError(f"{path}: unterminated YAML frontmatter") from error
    require(body.strip() != "", f"{path}: empty agent prompt")
    parsed: dict[str, object] = {}
    for number, line in enumerate(raw.splitlines(), 2):
        require(line and not line[0].isspace(), f"{path}:{number}: unsupported nested YAML")
        key, separator, value = line.partition(":")
        require(separator == ":" and key not in parsed, f"{path}:{number}: invalid or duplicate key")
        value = value.strip()
        if value.startswith("["):
            require(value.endswith("]"), f"{path}:{number}: malformed list")
            parsed[key] = [item.strip().strip('"\'') for item in value[1:-1].split(",") if item.strip()]
        elif value.startswith('"'):
            parsed[key] = json.loads(value)
        else:
            parsed[key] = value
    return parsed


def validate_agents() -> None:
    seen: set[str] = set()
    for path in sorted((ROOT / ".claude/agents").glob("*.md")):
        data = parse_frontmatter(path)
        name = str(data.get("name", ""))
        require(name in CLAUDE_AGENTS, f"{path}: unexpected agent name {name!r}")
        require(name not in seen, f"{path}: duplicate agent name")
        seen.add(name)
        model, effort, permission, writes = CLAUDE_AGENTS[name]
        require(data.get("model") == model, f"{path}: wrong model")
        require(data.get("effort") == effort, f"{path}: wrong effort")
        require(data.get("permissionMode") == permission, f"{path}: wrong permission mode")
        require(str(data.get("maxTurns", "")).isdigit(), f"{path}: maxTurns must be numeric")
        tools = set(data.get("tools", []))
        denied = set(data.get("disallowedTools", []))
        if writes:
            require({"Edit", "Write"} <= tools and "Bash" not in tools, f"{path}: unsafe draft tools")
            require(data.get("isolation") == "worktree", f"{path}: draft must use worktree isolation")
            require("Bash" in denied, f"{path}: draft must explicitly deny Bash")
        else:
            require(not ({"Bash", "Edit", "Write"} & tools), f"{path}: read-only agent exposes writes")
            require({"Bash", "Edit", "Write"} <= denied, f"{path}: read-only denylist incomplete")
    require(seen == set(CLAUDE_AGENTS), "Claude agent set is incomplete")

    seen.clear()
    for path in sorted((ROOT / ".codex/agents").glob("*.toml")):
        data = tomllib.loads(path.read_text())
        require({"name", "description", "developer_instructions"} <= data.keys(), f"{path}: missing key")
        name = data["name"]
        require(name in CODEX_AGENTS and name not in seen, f"{path}: unknown or duplicate agent")
        seen.add(name)
        model, effort, sandbox = CODEX_AGENTS[name]
        require(data.get("model") == model, f"{path}: wrong model")
        require(data.get("model_reasoning_effort") == effort, f"{path}: wrong effort")
        require(data.get("sandbox_mode") == sandbox, f"{path}: wrong sandbox")
    require(seen == set(CODEX_AGENTS), "Codex agent set is incomplete")


def validate_settings_and_schema() -> None:
    settings = json.loads((ROOT / ".claude/settings.json").read_text())
    require(settings.get("worktree", {}).get("baseRef") == "head", "Claude worktrees must use local HEAD")
    hooks = settings.get("hooks", {}).get("PreToolUse", [])
    require(any(group.get("matcher") == "Bash" for group in hooks), "release mutation hook is missing")
    commands = [hook.get("command", "") for group in hooks for hook in group.get("hooks", [])]
    require(any("deny-release-mutation.py" in command for command in commands), "hook script is not registered")
    require("Bash" in settings.get("permissions", {}).get("deny", []), "project settings must deny Claude Bash")

    schema = json.loads((ROOT / "scripts/ai/claude-handoff.schema.json").read_text())
    required = set(schema.get("required", []))
    require(
        {"status", "checks", "findings", "patch_sha256", "git_status", "next"} <= required,
        "Claude handoff schema omits fail-closed fields",
    )


def validate_wrapper() -> None:
    wrapper = ROOT / "scripts/ai/claude-consult.sh"
    syntax = run(["bash", "-n", os.fspath(wrapper)])
    require(syntax.returncode == 0, syntax.stderr)
    require(stat.S_IMODE(wrapper.stat().st_mode) & 0o111 != 0, "Claude wrapper is not executable")

    base_env = os.environ.copy()
    base_env["COMICCHAT_CLAUDE_DRY_RUN"] = "1"
    expected = {
        "inventory": ("haiku", "low", "plan"),
        "research": ("sonnet", "medium", "plan"),
        "review": ("opus", "high", "plan"),
        "security": ("opus", "high", "plan"),
    }
    for lane, (model, effort, permission) in expected.items():
        result = run([os.fspath(wrapper), lane, os.fspath(ROOT / "AGENTS.md")], env=base_env)
        require(result.returncode == 0, f"{lane} dry-run failed: {result.stderr}")
        for token in ("--safe-mode", "--no-session-persistence", "--strict-mcp-config", "--output-format json"):
            require(token in result.stdout, f"{lane}: missing {token}")
        require(f"--model {model}" in result.stdout, f"{lane}: wrong model")
        require(f"--effort {effort}" in result.stdout, f"{lane}: wrong effort")
        require(f"--permission-mode {permission}" in result.stdout, f"{lane}: wrong permission")
        tool_match = re.search(r"--tools ([^ ]+)", result.stdout)
        require(tool_match is not None and "Bash" not in tool_match.group(1), f"{lane}: Bash exposed")
        require("--setting-sources" not in result.stdout, f"{lane}: safe mode must not load user settings")

    git_dir = run(["git", "rev-parse", "--path-format=absolute", "--git-dir"]).stdout.strip()
    common_dir = run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"]).stdout.strip()
    draft = run([os.fspath(wrapper), "draft", os.fspath(ROOT / "AGENTS.md")], env=base_env)
    require(draft.returncode == (77 if git_dir == common_dir else 0), "draft worktree guard is wrong")

    validation_env = base_env.copy()
    validation_env["COMICCHAT_CLAUDE_VALIDATE_DRAFT"] = "1"
    draft = run([os.fspath(wrapper), "draft", os.fspath(ROOT / "AGENTS.md")], env=validation_env)
    require(draft.returncode == 0, draft.stderr)
    require("--model sonnet" in draft.stdout and "--effort medium" in draft.stdout, "draft routing is wrong")
    tool_match = re.search(r"--tools ([^ ]+)", draft.stdout)
    require(tool_match is not None and "Bash" not in tool_match.group(1), "draft exposes Bash")

    relative = run([os.fspath(wrapper), "inventory", "meson.build"], cwd=ROOT / "portable", env=base_env)
    require(relative.returncode == 0, f"relative prompt regression: {relative.stderr}")

    handoff = {
        "role": "research",
        "status": "complete",
        "summary": "fixture",
        "worktree": "not-run",
        "base": "not-run",
        "head": "not-run",
        "commit": "not-run",
        "git_status": "not-run",
        "diffstat": "not-run",
        "patch_sha256": "not-run",
        "scope": "fixture",
        "oracle": "fixture",
        "red": "not-applicable",
        "green": "not-applicable",
        "sanitizers": "not-applicable",
        "artifacts": "none",
        "checks": [],
        "findings": [],
        "risks": [],
        "next": "none",
    }
    with tempfile.TemporaryDirectory(prefix="comicchat-fake-claude-", dir=temporary_root()) as raw:
        directory = Path(raw)
        fake_bin = directory / "bin"
        fake_bin.mkdir()
        fake_claude = fake_bin / "claude"
        fake_claude.write_text("#!/bin/sh\ncat \"$COMICCHAT_FAKE_CLAUDE_RESPONSE\"\n")
        fake_claude.chmod(0o755)
        response = directory / "response.json"
        fake_env = os.environ.copy()
        fake_env["PATH"] = os.fspath(fake_bin) + os.pathsep + fake_env.get("PATH", "")
        fake_env["TMPDIR"] = os.fspath(directory)
        fake_env["COMICCHAT_FAKE_CLAUDE_RESPONSE"] = os.fspath(response)

        envelope = {
            "type": "result",
            "subtype": "success",
            "is_error": False,
            "structured_output": handoff,
        }
        response.write_text(json.dumps(envelope))
        result = run([os.fspath(wrapper), "inventory", os.fspath(ROOT / "AGENTS.md")], env=fake_env)
        require(result.returncode == 0, f"valid structured handoff failed: {result.stderr}")
        emitted = json.loads(result.stdout)
        require(emitted["worktree"] == os.fspath(ROOT), "wrapper did not replace worktree metadata")
        require(emitted["git_status"] != "not-run", "wrapper trusted Claude Git metadata")
        require(emitted["role"] == "research", "wrapper did not enforce lane role")

        partial = dict(envelope)
        partial["structured_output"] = {"status": "complete", "checks": []}
        response.write_text(json.dumps(partial))
        result = run([os.fspath(wrapper), "inventory", os.fspath(ROOT / "AGENTS.md")], env=fake_env)
        require(result.returncode == 65, "partial structured handoff escaped validation")

        error_envelope = dict(envelope)
        error_envelope.update({"subtype": "error_max_turns", "is_error": True})
        response.write_text(json.dumps(error_envelope))
        result = run([os.fspath(wrapper), "inventory", os.fspath(ROOT / "AGENTS.md")], env=fake_env)
        require(result.returncode == 65, "error Claude envelope escaped validation")

        inconsistent = json.loads(json.dumps(envelope))
        inconsistent["structured_output"]["checks"] = [
            {
                "kind": "model-tool",
                "tool": "Grep",
                "command": "Grep fixture",
                "exit_code": -1,
                "result": "passed",
                "evidence": "none",
            }
        ]
        response.write_text(json.dumps(inconsistent))
        result = run([os.fspath(wrapper), "inventory", os.fspath(ROOT / "AGENTS.md")], env=fake_env)
        require(result.returncode == 65, "inconsistent check result escaped validation")

        invented = json.loads(json.dumps(envelope))
        invented["structured_output"]["green"] = "meson test passed"
        invented["structured_output"]["checks"] = [
            {
                "kind": "proposed-command",
                "tool": "none",
                "command": "meson test -C build",
                "exit_code": 0,
                "result": "passed",
                "evidence": "all passed",
            }
        ]
        response.write_text(json.dumps(invented))
        result = run([os.fspath(wrapper), "inventory", os.fspath(ROOT / "AGENTS.md")], env=fake_env)
        require(result.returncode == 65, "invented shell verification escaped validation")

        prose_bypass = json.loads(json.dumps(envelope))
        prose_bypass["structured_output"].update(
            {
                "red": "not-run; baseline failed with exit 1",
                "green": "not-run; meson passed 100 tests",
                "sanitizers": "not-applicable but ASan passed",
            }
        )
        response.write_text(json.dumps(prose_bypass))
        result = run([os.fspath(wrapper), "inventory", os.fspath(ROOT / "AGENTS.md")], env=fake_env)
        require(result.returncode == 65, "verification prose escaped canonical evidence tokens")


def validate_hook() -> None:
    hook = ROOT / "scripts/ai/deny-release-mutation.py"
    blocked = [
        "git status --short",
        "git -C . diff --stat",
        "rg 'git push' docs",
        "git push origin HEAD",
        "git send-pack origin HEAD",
        "git update-ref refs/heads/main HEAD",
        "eval 'git push origin HEAD'",
        "echo \"$(git push origin HEAD)\"",
        "gh pr merge 1",
    ]
    for command in blocked:
        payload = json.dumps({"tool_name": "Bash", "tool_input": {"command": command}})
        result = run([sys.executable, os.fspath(hook)], input_text=payload)
        require(result.returncode == 2, f"Claude Bash escaped hook: {command}")
    non_bash = json.dumps({"tool_name": "Read", "tool_input": {"file_path": "AGENTS.md"}})
    require(run([sys.executable, os.fspath(hook)], input_text=non_bash).returncode == 0, "non-Bash tool blocked")
    for malformed in ("not-json", "[]", "null", '"x"', "{}"):
        require(
            run([sys.executable, os.fspath(hook)], input_text=malformed).returncode == 2,
            f"bad hook input passed: {malformed}",
        )


def validate_fingerprint() -> None:
    script = ROOT / "scripts/ai/worktree-fingerprint.py"
    with tempfile.TemporaryDirectory(prefix="comicchat-fingerprint-", dir=temporary_root()) as raw:
        repo = Path(raw)
        require(run(["git", "init", "-q"], cwd=repo).returncode == 0, "temporary git init failed")
        run(["git", "config", "user.name", "Workflow Test"], cwd=repo)
        run(["git", "config", "user.email", "workflow@example.invalid"], cwd=repo)
        (repo / "tracked.txt").write_text("baseline\n")
        run(["git", "add", "tracked.txt"], cwd=repo)
        require(run(["git", "commit", "-qm", "baseline"], cwd=repo).returncode == 0, "temporary commit failed")

        clean = run([sys.executable, os.fspath(script), "--repo", os.fspath(repo)])
        require(clean.returncode == 0 and re.fullmatch(r"[0-9a-f]{64}\n", clean.stdout), clean.stderr)
        (repo / "untracked.txt").write_text("one\n")
        first = run([sys.executable, os.fspath(script), "--repo", os.fspath(repo)])
        (repo / "untracked.txt").write_text("two\n")
        second = run([sys.executable, os.fspath(script), "--repo", os.fspath(repo)])
        require(first.returncode == second.returncode == 0, first.stderr + second.stderr)
        require(clean.stdout != first.stdout != second.stdout, "untracked content is absent from fingerprint")
        stable = run([sys.executable, os.fspath(script), "--repo", os.fspath(repo)])
        require(stable.stdout == second.stdout, "stable worktree fingerprint changed")

    with tempfile.TemporaryDirectory(prefix="comicchat-submodule-fingerprint-", dir=temporary_root()) as raw:
        directory = Path(raw)
        parent = directory / "parent"
        child = directory / "child"
        parent.mkdir()
        child.mkdir()
        for repo in (parent, child):
            require(run(["git", "init", "-q"], cwd=repo).returncode == 0, "temporary git init failed")
            run(["git", "config", "user.name", "Workflow Test"], cwd=repo)
            run(["git", "config", "user.email", "workflow@example.invalid"], cwd=repo)
        (parent / "root.txt").write_text("root\n")
        run(["git", "add", "root.txt"], cwd=parent)
        require(run(["git", "commit", "-qm", "parent"], cwd=parent).returncode == 0, "parent commit failed")
        (child / "child.txt").write_text("one\n")
        run(["git", "add", "child.txt"], cwd=child)
        require(run(["git", "commit", "-qm", "child"], cwd=child).returncode == 0, "child commit failed")
        added = run(
            ["git", "-c", "protocol.file.allow=always", "submodule", "add", os.fspath(child), "deps/child"],
            cwd=parent,
        )
        require(added.returncode == 0, f"submodule fixture failed: {added.stderr}")
        run(["git", "add", ".gitmodules", "deps/child"], cwd=parent)
        require(run(["git", "commit", "-qm", "submodule"], cwd=parent).returncode == 0, "submodule commit failed")
        clean = run([sys.executable, os.fspath(script), "--repo", os.fspath(parent)])
        require(clean.returncode == 0, f"clean submodule rejected: {clean.stderr}")
        deinitialized = run(["git", "submodule", "deinit", "-f", "deps/child"], cwd=parent)
        require(deinitialized.returncode == 0, f"submodule deinit failed: {deinitialized.stderr}")
        (parent / "top-level-draft.txt").write_text("draft\n")
        deinit_fingerprint = run([sys.executable, os.fspath(script), "--repo", os.fspath(parent)])
        require(deinit_fingerprint.returncode == 0, f"deinitialized submodule rejected: {deinit_fingerprint.stderr}")
        initialized = run(
            ["git", "-c", "protocol.file.allow=always", "submodule", "update", "--init", "deps/child"],
            cwd=parent,
        )
        require(initialized.returncode == 0, f"submodule reinit failed: {initialized.stderr}")
        (parent / "deps/child/child.txt").write_text("two\n")
        dirty = run([sys.executable, os.fspath(script), "--repo", os.fspath(parent)])
        require(dirty.returncode != 0, "dirty submodule bytes escaped fingerprint guard")
        require("dirty initialized submodule" in dirty.stderr, dirty.stderr)


def validate_ci_and_diff() -> None:
    workflow = (ROOT / ".github/workflows/agent-workflow.yml").read_text()
    for path in (".claude/**", ".codex/**", "AGENTS.md", "CLAUDE.md", "scripts/ai/**"):
        require(path in workflow, f"agent workflow path filter omits {path}")
    require("python3 scripts/ai/validate-workflow.py" in workflow, "CI does not run workflow validator")
    diff = run(["git", "diff", "--check"])
    require(diff.returncode == 0, diff.stdout + diff.stderr)


def main() -> int:
    try:
        validate_agents()
        validate_settings_and_schema()
        validate_wrapper()
        validate_hook()
        validate_fingerprint()
        validate_ci_and_diff()
    except (OSError, RuntimeError, subprocess.SubprocessError, tomllib.TOMLDecodeError) as error:
        print(f"workflow validation failed: {error}", file=sys.stderr)
        return 1
    print("Codex + Claude workflow validation passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
