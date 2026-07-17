#!/usr/bin/env python3
"""Inventory native chrome on every eligible Comic Chat dialog resource."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESOURCE = ROOT / "chat.rc"
SHEET_SOURCE = ROOT / "chicdial.cpp"
REQUIRED = ("WS_CAPTION", "WS_SYSMENU", "WS_MINIMIZEBOX")


def dialog_blocks(text: str) -> list[tuple[str, str]]:
    lines = text.splitlines()
    blocks: list[tuple[str, str]] = []
    for index, line in enumerate(lines):
        match = re.match(r"^(\S+)\s+DIALOG(?:EX)?\b", line)
        if not match:
            continue
        body: list[str] = []
        for following in lines[index + 1:]:
            if following.strip() == "BEGIN":
                break
            body.append(following.strip())
        blocks.append((match.group(1), " ".join(body)))
    return blocks


def main() -> int:
    failures: list[str] = []
    eligible: list[str] = []
    for resource, declaration in dialog_blocks(RESOURCE.read_text(encoding="cp1252")):
        if "WS_CHILD" in declaration:
            continue
        eligible.append(resource)
        missing = [style for style in REQUIRED if style not in declaration]
        if missing:
            failures.append(f"{resource}: missing {', '.join(missing)}")
        if "WS_EX_CONTEXTHELP" in declaration:
            failures.append(
                f"{resource}: WS_EX_CONTEXTHELP suppresses the native minimize box")

    sheet = SHEET_SOURCE.read_text(encoding="cp1252")
    for style in REQUIRED:
        if style not in sheet:
            failures.append(f"CCSPropertySheet runtime style missing {style}")
    if "dwExStyle &= ~WS_EX_CONTEXTHELP" not in sheet:
        failures.append("CCSPropertySheet does not remove minimize-incompatible context chrome")

    if failures:
        print("native dialog chrome audit failed")
        for failure in failures:
            print(f"- {failure}")
        return 1
    print(f"native dialog chrome audit passed: {len(eligible)} top-level resources")
    print("close=system menu/Alt+F4; cancel=Escape; minimize=native caption button")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
