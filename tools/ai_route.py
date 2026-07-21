#!/usr/bin/env python3
"""Deterministically route a ComicChat task to the model-scaled workflow."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
POLICY_PATH = ROOT / ".ai" / "model-routing.json"


def bounded_int(name: str, maximum: int):
    def parse(value: str) -> int:
        parsed = int(value)
        if not 0 <= parsed <= maximum:
            raise argparse.ArgumentTypeError(f"{name} must be between 0 and {maximum}")
        return parsed

    return parse


def load_policy() -> dict[str, Any]:
    with POLICY_PATH.open(encoding="utf-8") as handle:
        return json.load(handle)


def env_model(policy: dict[str, Any], lane: str) -> str:
    variable = policy["environment_overrides"][lane]
    return os.environ.get(variable, policy["models"][lane]["model"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scope", required=True, type=bounded_int("scope", 3))
    parser.add_argument("--ambiguity", required=True, type=bounded_int("ambiguity", 2))
    parser.add_argument("--risk", required=True, type=bounded_int("risk", 3))
    parser.add_argument("--verification", required=True, type=bounded_int("verification", 2))
    parser.add_argument("--lanes", type=int, default=1)
    parser.add_argument("--domain", default="")
    args = parser.parse_args()

    if args.lanes < 1:
        parser.error("lanes must be at least 1")

    policy = load_policy()
    score = args.scope + args.ambiguity + args.risk + args.verification
    routing = policy["routing"]
    if score >= routing["frontier_min_score"]:
        lane = "frontier"
    elif score >= routing["balanced_min_score"]:
        lane = "balanced"
    else:
        lane = "fast"

    critical = args.domain.lower() in routing["critical_domains"]
    reviewer = "frontier" if critical or lane == "frontier" else "fast"
    configured_max = int(
        os.environ.get(
            policy["environment_overrides"]["max_parallel"],
            routing["default_max_parallel"],
        )
    )
    max_parallel = max(1, configured_max)

    result = {
        "score": score,
        "primary_lane": lane,
        "primary_model": env_model(policy, lane),
        "reasoning_effort": policy["models"][lane]["reasoning_effort"],
        "review_lane": reviewer,
        "review_model": env_model(policy, reviewer),
        "critical_domain": critical,
        "parallel_lanes": min(args.lanes, max_parallel),
        "requested_lanes": args.lanes,
        "max_parallel": max_parallel,
        "policy": str(POLICY_PATH.relative_to(ROOT)),
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
