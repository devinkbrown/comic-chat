#!/usr/bin/env python3
"""Validate and emit Claude's structured handoff without third-party modules."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any


class ValidationError(ValueError):
    """Raised when an instance does not match the supported JSON Schema subset."""


def type_matches(instance: Any, expected: str) -> bool:
    if expected == "object":
        return isinstance(instance, dict)
    if expected == "array":
        return isinstance(instance, list)
    if expected == "string":
        return isinstance(instance, str)
    if expected == "integer":
        return isinstance(instance, int) and not isinstance(instance, bool)
    raise ValidationError(f"unsupported schema type {expected!r}")


def validate(instance: Any, schema: dict[str, Any], location: str = "$") -> None:
    expected = schema.get("type")
    if expected is not None:
        if not isinstance(expected, str) or not type_matches(instance, expected):
            raise ValidationError(f"{location}: expected {expected}")

    choices = schema.get("enum")
    if choices is not None:
        if not isinstance(choices, list) or instance not in choices:
            raise ValidationError(f"{location}: value is not in the permitted enum")

    if isinstance(instance, dict):
        properties = schema.get("properties", {})
        required = schema.get("required", [])
        if not isinstance(properties, dict) or not isinstance(required, list):
            raise ValidationError(f"{location}: invalid object schema")
        missing = [key for key in required if key not in instance]
        if missing:
            raise ValidationError(f"{location}: missing required fields: {', '.join(missing)}")
        if schema.get("additionalProperties") is False:
            extra = sorted(set(instance) - set(properties))
            if extra:
                raise ValidationError(f"{location}: unexpected fields: {', '.join(extra)}")
        for key, value in instance.items():
            child = properties.get(key)
            if child is not None:
                if not isinstance(child, dict):
                    raise ValidationError(f"{location}.{key}: invalid property schema")
                validate(value, child, f"{location}.{key}")

    if isinstance(instance, list):
        item_schema = schema.get("items")
        if item_schema is not None:
            if not isinstance(item_schema, dict):
                raise ValidationError(f"{location}: invalid item schema")
            for index, value in enumerate(instance):
                validate(value, item_schema, f"{location}[{index}]")


def load_json(path: Path, description: str) -> Any:
    try:
        return json.loads(path.read_text())
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"invalid {description}: {error}") from error


def git(repo: Path, *arguments: str) -> str:
    try:
        return subprocess.run(
            ["git", "-C", os.fspath(repo), *arguments],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        ).stdout.rstrip()
    except subprocess.CalledProcessError as error:
        detail = error.stderr.strip() or str(error)
        raise ValidationError(f"could not capture trusted Git metadata: {detail}") from error


def fingerprint(root: Path) -> str:
    result = subprocess.run(
        [sys.executable, os.fspath(root / "scripts/ai/worktree-fingerprint.py"), "--repo", os.fspath(root)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        raise ValidationError(f"could not fingerprint worktree: {result.stderr.strip()}")
    return result.stdout.strip()


def trusted_metadata(repo: Path, base: str, role: str, expected_fingerprint: str) -> dict[str, str]:
    root = Path(git(repo, "rev-parse", "--show-toplevel"))
    before = fingerprint(root)
    if before != expected_fingerprint:
        raise ValidationError("worktree fingerprint changed before trusted metadata capture")
    status = git(root, "status", "--short", "--untracked-files=all")
    diffstat = git(root, "diff", "--stat", "HEAD", "--", ".")
    head = git(root, "rev-parse", "HEAD")
    after = fingerprint(root)
    if after != before:
        raise ValidationError("worktree changed during trusted metadata capture")
    return {
        "role": role,
        "worktree": os.fspath(root),
        "base": base,
        "head": head,
        "commit": "none",
        "git_status": status or "clean",
        "diffstat": diffstat or "none",
        "patch_sha256": before,
    }


def validate_check_semantics(handoff: dict[str, Any], allowed_tools: set[str] | None) -> None:
    for field in ("red", "green", "sanitizers"):
        value = handoff[field].strip().lower()
        if value not in {"not-run", "not-applicable"}:
            raise ValidationError(f"$.{field}: expected exact token not-run or not-applicable")

    for index, check in enumerate(handoff["checks"]):
        kind = check["kind"]
        tool = check["tool"]
        command = check["command"].strip()
        result = check["result"]
        exit_code = check["exit_code"]
        if kind == "proposed-command":
            if tool != "none" or result != "not-run" or exit_code != -1:
                raise ValidationError(
                    f"$.checks[{index}]: proposed commands require tool none, not-run, and exit_code -1"
                )
            continue
        if tool == "none" or (allowed_tools is not None and tool not in allowed_tools):
            raise ValidationError(f"$.checks[{index}]: model tool was not available in this lane")
        if command != tool and not command.startswith(tool + " "):
            raise ValidationError(f"$.checks[{index}]: model-tool command must start with its exact tool name")
        if result == "not-run":
            raise ValidationError(f"$.checks[{index}]: an unexecuted operation is a proposed-command")
        if result == "passed" and exit_code != 0:
            raise ValidationError(f"$.checks[{index}]: passed requires exit_code 0")
        if result == "failed" and exit_code in {-1, 0}:
            raise ValidationError(f"$.checks[{index}]: failed requires a nonzero executed exit code")
        if result == "not-run" and exit_code != -1:
            raise ValidationError(f"$.checks[{index}]: not-run requires exit_code -1")


def validate_execution_prose(handoff: dict[str, Any]) -> None:
    subject = r"(?:tests?|build|compile|link|meson|nmake|asan|ubsan|tsan|sanitizers?|benchmarks?|ci)"
    outcome = r"(?:passed|succeeded|successful|green|clean|ran|executed|exit(?:\s+code)?\s*0)"
    patterns = (
        re.compile(rf"\b{subject}\b.{{0,48}}\b{outcome}\b", re.IGNORECASE | re.DOTALL),
        re.compile(rf"\b{outcome}\b.{{0,48}}\b{subject}\b", re.IGNORECASE | re.DOTALL),
    )
    fields: list[tuple[str, str]] = [
        ("summary", handoff["summary"]),
        ("artifacts", handoff["artifacts"]),
        ("next", handoff["next"]),
    ]
    fields.extend((f"risks[{index}]", value) for index, value in enumerate(handoff["risks"]))
    for location, value in fields:
        if any(pattern.search(value) for pattern in patterns):
            raise ValidationError(f"$.{location}: unsupported execution claim outside checks")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("envelope", type=Path)
    parser.add_argument("model_schema", type=Path)
    parser.add_argument("final_schema", type=Path)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--base", required=True)
    parser.add_argument(
        "--role",
        choices=["research", "implementation", "review", "verification"],
        required=True,
    )
    parser.add_argument("--expected-model", choices=["haiku", "sonnet", "opus"], required=True)
    parser.add_argument("--allowed-tools")
    parser.add_argument("--expected-fingerprint", required=True)
    arguments = parser.parse_args()

    try:
        envelope = load_json(arguments.envelope, "Claude JSON envelope")
        model_schema = load_json(arguments.model_schema, "Claude model handoff schema")
        final_schema = load_json(arguments.final_schema, "Claude final handoff schema")
        if not isinstance(envelope, dict):
            raise ValidationError("Claude JSON envelope must be an object")
        if (
            envelope.get("type") != "result"
            or envelope.get("subtype") != "success"
            or envelope.get("is_error") is not False
        ):
            raise ValidationError("Claude envelope is not a successful result")
        model_usage = envelope.get("modelUsage")
        if not isinstance(model_usage, dict) or not model_usage:
            raise ValidationError("Claude envelope omitted modelUsage")
        unexpected_models = sorted(
            model_name
            for model_name in model_usage
            if arguments.expected_model not in model_name.lower()
        )
        if unexpected_models:
            raise ValidationError(
                "Claude used a model outside the routed family: " + ", ".join(unexpected_models)
            )
        handoff = envelope.get("structured_output")
        if not isinstance(handoff, dict):
            raise ValidationError("Claude response omitted structured_output")
        if not isinstance(model_schema, dict) or not isinstance(final_schema, dict):
            raise ValidationError("Claude handoff schemas must be objects")
        validate(handoff, model_schema)
        handoff.update({"red": "not-run", "green": "not-run", "sanitizers": "not-run"})
        allowed_tools = None
        if arguments.allowed_tools is not None:
            allowed_tools = {tool for tool in arguments.allowed_tools.split(",") if tool}
        validate_check_semantics(handoff, allowed_tools)
        validate_execution_prose(handoff)
        handoff.update(
            trusted_metadata(arguments.repo, arguments.base, arguments.role, arguments.expected_fingerprint)
        )
        validate(handoff, final_schema)
    except ValidationError as error:
        print(f"Invalid Claude handoff: {error}", file=sys.stderr)
        return 65

    print(json.dumps(handoff, indent=2, sort_keys=True))
    failed_check = any(check["result"] == "failed" for check in handoff["checks"])
    return 0 if handoff["status"] == "complete" and not failed_check else 1


if __name__ == "__main__":
    raise SystemExit(main())
