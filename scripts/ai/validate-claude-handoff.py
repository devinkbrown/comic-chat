#!/usr/bin/env python3
"""Validate and emit Claude's structured handoff without third-party modules."""

from __future__ import annotations

import argparse
import json
import os
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


def trusted_metadata(repo: Path, base: str, role: str) -> dict[str, str]:
    root = Path(git(repo, "rev-parse", "--show-toplevel"))
    status = git(root, "status", "--short", "--untracked-files=all")
    diffstat = git(root, "diff", "--stat", "HEAD", "--", ".")
    fingerprint = subprocess.run(
        [sys.executable, os.fspath(root / "scripts/ai/worktree-fingerprint.py"), "--repo", os.fspath(root)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if fingerprint.returncode != 0:
        raise ValidationError(f"could not fingerprint worktree: {fingerprint.stderr.strip()}")
    return {
        "role": role,
        "worktree": os.fspath(root),
        "base": base,
        "head": git(root, "rev-parse", "HEAD"),
        "commit": "none",
        "git_status": status or "clean",
        "diffstat": diffstat or "none",
        "patch_sha256": fingerprint.stdout.strip(),
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("envelope", type=Path)
    parser.add_argument("schema", type=Path)
    parser.add_argument("--repo", type=Path)
    parser.add_argument("--base")
    parser.add_argument("--role", choices=["research", "implementation", "review", "verification"])
    parser.add_argument("--allowed-tools")
    arguments = parser.parse_args()
    if any(value is not None for value in (arguments.repo, arguments.base, arguments.role)) and any(
        value is None for value in (arguments.repo, arguments.base, arguments.role)
    ):
        parser.error("--repo, --base, and --role must be supplied together")

    try:
        envelope = load_json(arguments.envelope, "Claude JSON envelope")
        schema = load_json(arguments.schema, "Claude handoff schema")
        if not isinstance(envelope, dict):
            raise ValidationError("Claude JSON envelope must be an object")
        if (
            envelope.get("type") != "result"
            or envelope.get("subtype") != "success"
            or envelope.get("is_error") is not False
        ):
            raise ValidationError("Claude envelope is not a successful result")
        handoff = envelope.get("structured_output")
        if not isinstance(handoff, dict):
            raise ValidationError("Claude response omitted structured_output")
        if not isinstance(schema, dict):
            raise ValidationError("Claude handoff schema must be an object")
        validate(handoff, schema)
        allowed_tools = None
        if arguments.allowed_tools is not None:
            allowed_tools = {tool for tool in arguments.allowed_tools.split(",") if tool}
        validate_check_semantics(handoff, allowed_tools)
        if arguments.repo is not None:
            handoff.update(trusted_metadata(arguments.repo, arguments.base, arguments.role))
            validate(handoff, schema)
    except ValidationError as error:
        print(f"Invalid Claude handoff: {error}", file=sys.stderr)
        return 65

    print(json.dumps(handoff, indent=2, sort_keys=True))
    failed_check = any(check["result"] == "failed" for check in handoff["checks"])
    return 0 if handoff["status"] == "complete" and not failed_check else 1


if __name__ == "__main__":
    raise SystemExit(main())
