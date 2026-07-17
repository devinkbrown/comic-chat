#!/usr/bin/env python3
"""Keep Claude out of the shell; Codex owns execution and release state."""

from __future__ import annotations

import json
import sys


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, OSError, TypeError) as error:
        print(f"Blocked by Comic Chat execution policy: invalid hook input: {error}", file=sys.stderr)
        return 2

    if not isinstance(payload, dict):
        print("Blocked by Comic Chat execution policy: hook input must be an object", file=sys.stderr)
        return 2
    tool_name = payload.get("tool_name")
    if not isinstance(tool_name, str) or not tool_name:
        print("Blocked by Comic Chat execution policy: hook input omitted tool_name", file=sys.stderr)
        return 2
    if tool_name != "Bash":
        return 0
    print(
        "Blocked by Comic Chat execution policy: Claude does not run shell commands; "
        "return the proposed command for Codex to execute.",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
