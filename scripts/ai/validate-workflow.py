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
ROSTER_PATH = ROOT / "scripts/ai/agent-roster.json"
CLAUDE_FRONTMATTER_KEYS = {
    "name",
    "description",
    "tools",
    "disallowedTools",
    "skills",
    "model",
    "effort",
    "permissionMode",
    "isolation",
    "maxTurns",
}
CODEX_AGENT_KEYS = {
    "name",
    "description",
    "model",
    "model_reasoning_effort",
    "sandbox_mode",
    "nickname_candidates",
    "developer_instructions",
}
SKILL_NAMES = {
    "comicchat-cpp26-engineering",
    "comicchat-transport-security",
    "comicchat-ircv3-compat",
    "comicchat-render-fidelity",
    "comicchat-native-platforms",
    "comicchat-native-ui",
    "comicchat-icon-assets",
    "comicchat-performance",
    "comicchat-verification-release",
}


def load_roster() -> dict[str, object]:
    roster = json.loads(ROSTER_PATH.read_text())
    require(
        set(roster) == {"version", "claude_cli", "claude", "codex", "lanes"},
        "agent roster has unknown keys",
    )
    require(roster["version"] == 1, "unsupported agent roster version")
    return roster


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


def validate_agents(roster: dict[str, object]) -> None:
    claude_agents = roster["claude"]
    codex_agents = roster["codex"]
    require(isinstance(claude_agents, dict) and isinstance(codex_agents, dict), "agent maps must be objects")

    cli_policy = roster["claude_cli"]
    require(isinstance(cli_policy, dict), "Claude CLI policy must be an object")
    structured_output_tool = cli_policy["structured_output_tool"]
    expected_claude_files = {str(config["file"]) for config in claude_agents.values()}
    actual_claude_files = {path.name for path in (ROOT / ".claude/agents").glob("*.md")}
    require(actual_claude_files == expected_claude_files, "Claude agent filenames differ from the roster")
    for name, config in claude_agents.items():
        require(
            set(config)
            == {
                "file",
                "model",
                "effort",
                "permission_mode",
                "max_turns",
                "tools",
                "disallowed_tools",
                "preload_skills",
                "isolation",
                "writer",
            },
            f"Claude roster entry {name}: unexpected keys",
        )
        require(config["model"] in {"haiku", "sonnet", "opus"}, f"Claude roster entry {name}: bad model")
        require(config["effort"] in {"low", "medium", "high"}, f"Claude roster entry {name}: bad effort")
        require(config["permission_mode"] in {"default", "plan", "acceptEdits"}, f"Claude roster entry {name}: bad mode")
        require(isinstance(config["max_turns"], int) and 1 <= config["max_turns"] <= 100, f"{name}: bad max_turns")
        tools = config["tools"]
        denied = config["disallowed_tools"]
        preload_skills = config["preload_skills"]
        require(isinstance(tools, list) and len(tools) == len(set(tools)), f"{name}: duplicate tools")
        require(isinstance(denied, list) and len(denied) == len(set(denied)), f"{name}: duplicate denied tools")
        require(
            isinstance(preload_skills, list)
            and preload_skills
            and len(preload_skills) == len(set(preload_skills))
            and set(preload_skills) <= SKILL_NAMES,
            f"{name}: invalid preloaded skills",
        )
        require("Bash" not in tools and "Bash" in denied, f"{name}: Bash policy is not fail-closed")
        require(structured_output_tool in tools, f"{name}: structured handoff tool is not allowlisted")
        if config["writer"]:
            require(config["permission_mode"] == "acceptEdits", f"{name}: writer mode is wrong")
            require({"Edit", "Write"} <= set(tools), f"{name}: writer tools incomplete")
            require(config["isolation"] == "worktree", f"{name}: writer lacks worktree isolation")
        else:
            expected_mode = "default" if config["model"] == "haiku" else "plan"
            require(config["permission_mode"] == expected_mode, f"{name}: reviewer mode is wrong")
            require(not ({"Edit", "Write"} & set(tools)), f"{name}: read-only agent exposes writes")
            require({"Edit", "Write"} <= set(denied), f"{name}: read-only denylist incomplete")
            require(config["isolation"] is None, f"{name}: read-only agent has unexpected isolation")

        path = ROOT / ".claude/agents" / str(config["file"])
        data = parse_frontmatter(path)
        allowed_keys = CLAUDE_FRONTMATTER_KEYS if config["writer"] else CLAUDE_FRONTMATTER_KEYS - {"isolation"}
        require(set(data) == allowed_keys, f"{path}: frontmatter keys differ from policy")
        require(data["name"] == name and path.stem == name, f"{path}: filename/name mismatch")
        require(isinstance(data["description"], str) and data["description"].strip(), f"{path}: empty description")
        require(data["model"] == config["model"], f"{path}: wrong model")
        require(data["effort"] == config["effort"], f"{path}: wrong effort")
        require(data["permissionMode"] == config["permission_mode"], f"{path}: wrong permission mode")
        require(data["tools"] == config["tools"], f"{path}: tools differ from roster")
        require(data["disallowedTools"] == config["disallowed_tools"], f"{path}: denied tools differ from roster")
        require(data["skills"] == preload_skills, f"{path}: preloaded skills differ from roster")
        require(str(data["maxTurns"]) == str(config["max_turns"]), f"{path}: wrong maxTurns")
        if config["writer"]:
            require(data["isolation"] == config["isolation"], f"{path}: wrong isolation")
        body = path.read_text().split("\n---\n", 1)[1]
        for marker in ("docs/CPP26-ENGINEERING.md", "HANDOFF"):
            require(marker in body, f"{path}: prompt omits {marker}")
        require("correct repository" in body.lower() or "confirm the repository" in body.lower(), f"{path}: no repo check")
        for forbidden in ("merge", "push", "publish"):
            require(forbidden in body.lower(), f"{path}: prompt omits {forbidden} prohibition")
        if config["writer"]:
            require("isolated worktree" in body.lower(), f"{path}: writer omits isolated worktree")
        else:
            require("never edit" in body.lower() or "do not edit" in body.lower(), f"{path}: reviewer edit ban missing")

    expected_codex_files = {str(config["file"]) for config in codex_agents.values()}
    actual_codex_files = {path.name for path in (ROOT / ".codex/agents").glob("*.toml")}
    require(actual_codex_files == expected_codex_files, "Codex agent filenames differ from the roster")
    nicknames: set[str] = set()
    for name, config in codex_agents.items():
        require(set(config) == {"file", "model", "effort", "sandbox_mode"}, f"Codex roster entry {name}: unexpected keys")
        require(config["model"] in {"gpt-5.6", "gpt-5.6-terra"}, f"{name}: bad Codex model")
        require(config["effort"] in {"low", "medium", "high"}, f"{name}: bad Codex effort")
        require(config["sandbox_mode"] in {"read-only", "workspace-write"}, f"{name}: bad Codex sandbox")
        path = ROOT / ".codex/agents" / str(config["file"])
        data = tomllib.loads(path.read_text())
        require(set(data) == CODEX_AGENT_KEYS, f"{path}: unsupported or missing TOML keys")
        require(data["name"] == name and path.stem == name, f"{path}: filename/name mismatch")
        require(data["model"] == config["model"], f"{path}: wrong model")
        require(data["model_reasoning_effort"] == config["effort"], f"{path}: wrong effort")
        require(data["sandbox_mode"] == config["sandbox_mode"], f"{path}: wrong sandbox")
        require(isinstance(data["description"], str) and data["description"].strip(), f"{path}: empty description")
        candidates = data["nickname_candidates"]
        require(isinstance(candidates, list) and candidates and len(candidates) == len(set(candidates)), f"{path}: nicknames invalid")
        require(not (set(candidates) & nicknames), f"{path}: nickname reused across agents")
        nicknames.update(candidates)
        prompt = data["developer_instructions"]
        for marker in ("docs/CPP26-ENGINEERING.md", "HANDOFF"):
            require(marker in prompt, f"{path}: prompt omits {marker}")
        require("repository" in prompt.lower(), f"{path}: no repository confirmation")
        for forbidden in ("merge", "push", "publish"):
            require(forbidden in prompt.lower(), f"{path}: prompt omits {forbidden} prohibition")
        if config["sandbox_mode"] == "read-only":
            require("do not edit" in prompt.lower() or "read-only" in prompt.lower(), f"{path}: reviewer edit ban missing")
        else:
            require("worktree" in prompt.lower(), f"{path}: writable agent omits worktree boundary")


def validate_roster_and_routes(roster: dict[str, object]) -> None:
    lanes = roster["lanes"]
    claude_agents = roster["claude"]
    require(isinstance(lanes, dict) and lanes, "lane map must be a non-empty object")
    routed_agents: set[str] = set()
    route_script = ROOT / "scripts/ai/agent-route.py"
    require(stat.S_IMODE(route_script.stat().st_mode) & 0o111 != 0, "agent route helper is not executable")
    cli_policy = roster["claude_cli"]
    require(
        set(cli_policy) == {"minimum_version", "tested_version", "structured_output_tool"},
        "Claude CLI policy has unexpected keys",
    )
    version_pattern = re.compile(r"\d+\.\d+\.\d+")
    minimum_version = cli_policy["minimum_version"]
    tested_version = cli_policy["tested_version"]
    require(
        isinstance(minimum_version, str)
        and isinstance(tested_version, str)
        and version_pattern.fullmatch(minimum_version) is not None
        and version_pattern.fullmatch(tested_version) is not None,
        "Claude CLI versions must be numeric semantic versions",
    )
    require(cli_policy["structured_output_tool"] == "StructuredOutput", "structured-output token drifted")
    reported_minimum = run([sys.executable, os.fspath(route_script), "--minimum-cli-version"])
    require(reported_minimum.returncode == 0, reported_minimum.stderr)
    require(reported_minimum.stdout.strip() == minimum_version, "minimum Claude CLI version route drifted")
    for version in (minimum_version, tested_version, "999.0.0"):
        compatible = run([sys.executable, os.fspath(route_script), "--check-cli-version", version])
        require(compatible.returncode == 0, f"compatible Claude CLI {version} was rejected")
    older_parts = [int(part) for part in minimum_version.split(".")]
    require(older_parts[2] > 0, "minimum Claude CLI fixture requires a positive patch version")
    older_parts[2] -= 1
    older = ".".join(str(part) for part in older_parts)
    require(
        run([sys.executable, os.fspath(route_script), "--check-cli-version", older]).returncode != 0,
        "older Claude CLI escaped the compatibility guard",
    )
    require(
        run([sys.executable, os.fspath(route_script), "--check-cli-version", "not-a-version"]).returncode != 0,
        "malformed Claude CLI version escaped the compatibility guard",
    )
    listed = run([sys.executable, os.fspath(route_script), "--list"])
    require(listed.returncode == 0, listed.stderr)
    require(listed.stdout.splitlines() == list(lanes), "agent route list differs from manifest order")
    for lane_name, lane in lanes.items():
        require(set(lane) == {"agent", "role", "skills", "contract"}, f"lane {lane_name}: unexpected keys")
        require(lane["agent"] in claude_agents, f"lane {lane_name}: unknown Claude agent")
        require(lane["role"] in {"research", "implementation", "review", "verification"}, f"lane {lane_name}: bad role")
        require(isinstance(lane["skills"], list) and lane["skills"], f"lane {lane_name}: skills missing")
        require(set(lane["skills"]) <= SKILL_NAMES, f"lane {lane_name}: unknown skill")
        require(isinstance(lane["contract"], str) and lane["contract"].strip(), f"lane {lane_name}: empty contract")
        routed_agents.add(lane["agent"])
        config = claude_agents[lane["agent"]]
        expected = [
            lane["agent"],
            config["model"],
            config["effort"],
            config["permission_mode"],
            ",".join(config["tools"]),
            ",".join(config["disallowed_tools"]),
            str(config["max_turns"]),
            "1" if config["writer"] else "0",
            lane["role"],
            ",".join(lane["skills"]),
            lane["contract"],
        ]
        result = run([sys.executable, os.fspath(route_script), lane_name])
        require(result.returncode == 0, f"lane {lane_name}: route helper failed: {result.stderr}")
        require(result.stdout.rstrip("\n").split("\t") == expected, f"lane {lane_name}: route helper drift")
    require(routed_agents == set(claude_agents), "one or more Claude agents are unreachable from lanes")
    unknown = run([sys.executable, os.fspath(route_script), "not-a-lane"])
    require(unknown.returncode == 64, "unknown route is not rejected")


def parse_openai_interface(path: Path) -> dict[str, str]:
    lines = path.read_text().splitlines()
    require(lines and lines[0] == "interface:", f"{path}: expected only an interface mapping")
    parsed: dict[str, str] = {}
    for number, line in enumerate(lines[1:], 2):
        require(line.startswith("  ") and not line.startswith("    "), f"{path}:{number}: bad indentation")
        key, separator, raw_value = line.strip().partition(":")
        require(separator == ":" and key not in parsed, f"{path}:{number}: bad or duplicate field")
        raw_value = raw_value.strip()
        require(raw_value.startswith('"') and raw_value.endswith('"'), f"{path}:{number}: strings must be quoted")
        parsed[key] = json.loads(raw_value)
    return parsed


def validate_skills() -> None:
    canonical_root = ROOT / ".agents/skills"
    claude_root = ROOT / ".claude/skills"
    actual_canonical = {path.name for path in canonical_root.iterdir() if path.is_dir()}
    actual_claude = {path.name for path in claude_root.iterdir() if path.is_dir()}
    require(actual_canonical == SKILL_NAMES, "canonical skill directory set differs from policy")
    require(actual_claude == SKILL_NAMES, "Claude skill adapter set differs from policy")

    for name in sorted(SKILL_NAMES):
        skill_dir = canonical_root / name
        skill_path = skill_dir / "SKILL.md"
        metadata_path = skill_dir / "agents/openai.yaml"
        references_dir = skill_dir / "references"
        require(skill_path.is_file() and metadata_path.is_file(), f"{name}: canonical skill package incomplete")
        require(references_dir.is_dir(), f"{name}: references directory missing")
        references = sorted(references_dir.glob("*.md"))
        require(references, f"{name}: no focused reference material")

        for path in skill_dir.rglob("*"):
            require(not path.is_symlink(), f"{path}: skill packages may not contain symlinks")
            if path.is_file():
                require(stat.S_IMODE(path.stat().st_mode) & 0o111 == 0, f"{path}: unexpected executable skill content")
        canonical = parse_frontmatter(skill_path)
        require(set(canonical) == {"name", "description"}, f"{skill_path}: unsupported frontmatter")
        require(canonical["name"] == name, f"{skill_path}: directory/name mismatch")
        description = canonical["description"]
        require(isinstance(description, str) and 40 <= len(description) <= 1024, f"{skill_path}: bad description")
        canonical_text = skill_path.read_text()
        require("[TODO" not in canonical_text and "TODO:" not in canonical_text, f"{skill_path}: template text remains")
        require(not re.search(r"(?m)(?:^|[ \t])!`|^```!", canonical_text), f"{skill_path}: shell preprocessing is forbidden")
        for reference in references:
            require(f"references/{reference.name}" in canonical_text, f"{skill_path}: does not route {reference.name}")
            reference_text = reference.read_text()
            require(reference_text.strip(), f"{reference}: empty reference")
            require(not re.search(r"(?m)(?:^|[ \t])!`|^```!", reference_text), f"{reference}: shell preprocessing is forbidden")

        interface = parse_openai_interface(metadata_path)
        require(
            set(interface) == {"display_name", "short_description", "default_prompt"},
            f"{metadata_path}: interface fields differ from policy",
        )
        require(1 <= len(interface["display_name"]) <= 64, f"{metadata_path}: display name length invalid")
        require(25 <= len(interface["short_description"]) <= 64, f"{metadata_path}: short description length invalid")
        require(f"${name}" in interface["default_prompt"], f"{metadata_path}: default prompt omits skill name")

        adapter_path = claude_root / name / "SKILL.md"
        adapter = parse_frontmatter(adapter_path)
        require(set(adapter) == {"name", "description"}, f"{adapter_path}: adapter frontmatter must stay thin")
        require(adapter == canonical, f"{adapter_path}: adapter metadata differs from canonical skill")
        adapter_text = adapter_path.read_text()
        canonical_pointer = f"../../../.agents/skills/{name}/SKILL.md"
        require(canonical_pointer in adapter_text, f"{adapter_path}: canonical pointer is wrong")
        require(len(adapter_text.splitlines()) <= 12, f"{adapter_path}: adapter duplicates canonical procedure")
        require("allowed-tools" not in adapter_text.lower(), f"{adapter_path}: adapter escalates tool access")
        require(not re.search(r"(?m)(?:^|[ \t])!`|^```!", adapter_text), f"{adapter_path}: shell preprocessing is forbidden")


def validate_settings_and_schema() -> None:
    settings = json.loads((ROOT / ".claude/settings.json").read_text())
    require(settings.get("disableSkillShellExecution") is True, "Claude skill shell preprocessing must be disabled")
    require(settings.get("worktree", {}).get("baseRef") == "head", "Claude worktrees must use local HEAD")
    hooks = settings.get("hooks", {}).get("PreToolUse", [])
    require(any(group.get("matcher") == "Bash" for group in hooks), "release mutation hook is missing")
    commands = [hook.get("command", "") for group in hooks for hook in group.get("hooks", [])]
    require(any("deny-release-mutation.py" in command for command in commands), "hook script is not registered")
    require("Bash" in settings.get("permissions", {}).get("deny", []), "project settings must deny Claude Bash")

    model_schema = json.loads((ROOT / "scripts/ai/claude-model-handoff.schema.json").read_text())
    final_schema = json.loads((ROOT / "scripts/ai/claude-handoff.schema.json").read_text())
    model_required = set(model_schema.get("required", []))
    final_required = set(final_schema.get("required", []))
    require(
        model_required
        == {"status", "summary", "scope", "oracle", "artifacts", "checks", "findings", "risks", "next"},
        "Claude model handoff schema is not the compact untrusted contract",
    )
    trusted_fields = {"role", "worktree", "base", "head", "commit", "git_status", "diffstat", "patch_sha256"}
    execution_fields = {"red", "green", "sanitizers"}
    require(
        not ((trusted_fields | execution_fields) & set(model_schema.get("properties", {}))),
        "Claude model schema exposes locally supplied fields",
    )
    require(
        model_required | trusted_fields | execution_fields == final_required,
        "Claude handoff schema omits fail-closed fields",
    )
    for field in model_required:
        require(
            model_schema["properties"][field] == final_schema["properties"][field],
            f"Claude handoff schema disagrees on shared field {field}",
        )


def validate_wrapper(roster: dict[str, object]) -> None:
    wrapper = ROOT / "scripts/ai/claude-consult.sh"
    syntax = run(["bash", "-n", os.fspath(wrapper)])
    require(syntax.returncode == 0, syntax.stderr)
    require(stat.S_IMODE(wrapper.stat().st_mode) & 0o111 != 0, "Claude wrapper is not executable")

    base_env = os.environ.copy()
    base_env["COMICCHAT_CLAUDE_DRY_RUN"] = "1"
    validation_env = base_env.copy()
    validation_env["COMICCHAT_CLAUDE_VALIDATE_WRITER"] = "1"
    for lane_name, lane in roster["lanes"].items():
        agent = roster["claude"][lane["agent"]]
        result = run([os.fspath(wrapper), lane_name, os.fspath(ROOT / "AGENTS.md")], env=validation_env)
        require(result.returncode == 0, f"{lane_name} dry-run failed: {result.stderr}")
        for token in (
            "--setting-sources project",
            "--no-session-persistence",
            "--strict-mcp-config",
            "--no-chrome",
            "--output-format json",
        ):
            require(token in result.stdout, f"{lane_name}: missing {token}")
        require("--safe-mode" not in result.stdout, f"{lane_name}: safe mode disables project agents and skills")
        require("--disable-slash-commands" not in result.stdout, f"{lane_name}: project skills are disabled")
        require(f"--agent {lane['agent']}" in result.stdout, f"{lane_name}: wrong custom agent")
        require(f"--model {agent['model']}" in result.stdout, f"{lane_name}: wrong model")
        require(f"--effort {agent['effort']}" in result.stdout, f"{lane_name}: wrong effort")
        require(f"--permission-mode {agent['permission_mode']}" in result.stdout, f"{lane_name}: wrong permission")
        require(f"--max-turns {agent['max_turns']}" in result.stdout, f"{lane_name}: wrong turn cap")
        tool_match = re.search(r"--tools ([^ ]+)", result.stdout)
        require(tool_match is not None and "Bash" not in tool_match.group(1), f"{lane_name}: Bash exposed")
        denied_match = re.search(r"--disallowed-tools ([^ ]+)", result.stdout)
        require(denied_match is not None and "Bash" in denied_match.group(1), f"{lane_name}: Bash deny omitted")
        for skill in lane["skills"]:
            require(skill in result.stdout, f"{lane_name}: routed skill {skill} absent from contract")

    git_dir = run(["git", "rev-parse", "--path-format=absolute", "--git-dir"]).stdout.strip()
    common_dir = run(["git", "rev-parse", "--path-format=absolute", "--git-common-dir"]).stdout.strip()
    for lane_name, lane in roster["lanes"].items():
        if not roster["claude"][lane["agent"]]["writer"]:
            continue
        writer = run([os.fspath(wrapper), lane_name, os.fspath(ROOT / "AGENTS.md")], env=base_env)
        require(writer.returncode == (77 if git_dir == common_dir else 0), f"{lane_name}: worktree guard is wrong")

    relative = run([os.fspath(wrapper), "inventory", "meson.build"], cwd=ROOT / "portable", env=base_env)
    require(relative.returncode == 0, f"relative prompt regression: {relative.stderr}")

    with tempfile.TemporaryDirectory(prefix="comicchat-wrong-repo-", dir=temporary_root()) as raw:
        wrong_repo = Path(raw)
        require(run(["git", "init", "-q"], cwd=wrong_repo).returncode == 0, "wrong-repo fixture failed")
        refused = run([os.fspath(wrapper), "inventory", os.fspath(ROOT / "AGENTS.md")], cwd=wrong_repo, env=base_env)
        require(refused.returncode == 69, "Claude wrapper accepted a different git repository")

    handoff = {
        "status": "complete",
        "summary": "fixture",
        "scope": "fixture",
        "oracle": "fixture",
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
        fake_claude.write_text(
            "#!/bin/sh\n"
            "if [ \"${1:-}\" = \"--version\" ]; then\n"
            "  printf '2.1.212 (Claude Code)\\n'\n"
            "else\n"
            "  cat \"$COMICCHAT_FAKE_CLAUDE_RESPONSE\"\n"
            "fi\n"
        )
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
            "modelUsage": {"claude-haiku-fixture": {}},
            "structured_output": handoff,
        }
        response.write_text(json.dumps(envelope))
        result = run([os.fspath(wrapper), "inventory", os.fspath(ROOT / "AGENTS.md")], env=fake_env)
        require(result.returncode == 0, f"valid structured handoff failed: {result.stderr}")
        emitted = json.loads(result.stdout)
        require(emitted["worktree"] == os.fspath(ROOT), "wrapper did not replace worktree metadata")
        require(emitted["git_status"] != "not-run", "wrapper trusted Claude Git metadata")
        require(emitted["role"] == "research", "wrapper did not enforce lane role")

        model_drift = json.loads(json.dumps(envelope))
        model_drift["modelUsage"] = {"claude-sonnet-fixture": {}}
        response.write_text(json.dumps(model_drift))
        result = run([os.fspath(wrapper), "inventory", os.fspath(ROOT / "AGENTS.md")], env=fake_env)
        require(result.returncode == 65, "unexpected Claude model family escaped validation")
        response.write_text(json.dumps(envelope))

        wrong_fingerprint = run(
            [
                sys.executable,
                os.fspath(ROOT / "scripts/ai/validate-claude-handoff.py"),
                os.fspath(response),
                os.fspath(ROOT / "scripts/ai/claude-model-handoff.schema.json"),
                os.fspath(ROOT / "scripts/ai/claude-handoff.schema.json"),
                "--repo",
                os.fspath(ROOT),
                "--base",
                run(["git", "rev-parse", "HEAD"]).stdout.strip(),
                "--role",
                "research",
                "--expected-model",
                "haiku",
                "--allowed-tools",
                "Read,Grep,Glob",
                "--expected-fingerprint",
                "0" * 64,
            ]
        )
        require(wrong_fingerprint.returncode == 65, "mismatched trusted fingerprint escaped validation")

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

        trusted_spoof = json.loads(json.dumps(envelope))
        trusted_spoof["structured_output"]["git_status"] = "clean"
        response.write_text(json.dumps(trusted_spoof))
        result = run([os.fspath(wrapper), "inventory", os.fspath(ROOT / "AGENTS.md")], env=fake_env)
        require(result.returncode == 65, "model-supplied trusted metadata escaped compact schema")

        summary_claim = json.loads(json.dumps(envelope))
        summary_claim["structured_output"]["summary"] = "The build passed and all tests were green."
        response.write_text(json.dumps(summary_claim))
        result = run([os.fspath(wrapper), "inventory", os.fspath(ROOT / "AGENTS.md")], env=fake_env)
        require(result.returncode == 65, "unsupported execution claim escaped through summary prose")


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
    for path in (
        ".agents/**",
        ".claude/**",
        ".codex/**",
        "AGENTS.md",
        "CLAUDE.md",
        "docs/AI-DEVELOPMENT-WORKFLOW.md",
        "docs/CPP26-ENGINEERING.md",
        "scripts/ai/**",
    ):
        require(path in workflow, f"agent workflow path filter omits {path}")
    require("python3 scripts/ai/validate-workflow.py" in workflow, "CI does not run workflow validator")
    diff = run(["git", "diff", "--check"])
    require(diff.returncode == 0, diff.stdout + diff.stderr)


def main() -> int:
    try:
        roster = load_roster()
        validate_agents(roster)
        validate_roster_and_routes(roster)
        validate_skills()
        validate_settings_and_schema()
        validate_wrapper(roster)
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
