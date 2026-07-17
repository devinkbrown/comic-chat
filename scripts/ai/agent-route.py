#!/usr/bin/env python3
"""Resolve a Claude lane from the single repository agent roster."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("lane", nargs="?")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--minimum-cli-version", action="store_true")
    parser.add_argument("--check-cli-version")
    arguments = parser.parse_args()
    roster = json.loads((ROOT / "scripts/ai/agent-roster.json").read_text())
    cli_policy = roster["claude_cli"]
    if arguments.minimum_cli_version:
        print(cli_policy["minimum_version"])
        return 0
    if arguments.check_cli_version is not None:
        pattern = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")
        actual_match = pattern.fullmatch(arguments.check_cli_version)
        minimum_match = pattern.fullmatch(cli_policy["minimum_version"])
        if actual_match is None or minimum_match is None:
            print(f"unsupported Claude CLI version: {arguments.check_cli_version}", file=sys.stderr)
            return 65
        actual = tuple(int(part) for part in actual_match.groups())
        minimum = tuple(int(part) for part in minimum_match.groups())
        if actual < minimum:
            print(
                f"Claude CLI {arguments.check_cli_version} is older than the tested minimum "
                f"{cli_policy['minimum_version']}",
                file=sys.stderr,
            )
            return 69
        return 0
    lanes = roster["lanes"]
    if arguments.list:
        for name in lanes:
            print(name)
        return 0
    if not arguments.lane or arguments.lane not in lanes:
        print(f"unknown Claude lane: {arguments.lane or ''}", file=sys.stderr)
        return 64
    lane = lanes[arguments.lane]
    agent = roster["claude"][lane["agent"]]
    values = (
        lane["agent"],
        agent["model"],
        agent["effort"],
        agent["permission_mode"],
        ",".join(agent["tools"]),
        ",".join(agent["disallowed_tools"]),
        str(agent["max_turns"]),
        "1" if agent["writer"] else "0",
        lane["role"],
        ",".join(lane["skills"]),
        lane["contract"],
    )
    if any("\t" in value or "\n" in value for value in values):
        print("agent route contains an unsafe delimiter", file=sys.stderr)
        return 65
    print("\t".join(values))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
