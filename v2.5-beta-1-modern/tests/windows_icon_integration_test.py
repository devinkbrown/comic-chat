#!/usr/bin/env python3
"""Audit Windows resource rebuilds and per-DPI window icon selection."""

from __future__ import annotations

import re
from pathlib import Path


MODERN = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    rc = (MODERN / "chat.rc").read_text(encoding="cp1252")
    makefile = (MODERN / "chat.mak").read_text(encoding="cp1252")
    loader = (MODERN / "modernicons.cpp").read_text(encoding="cp1252")
    header = (MODERN / "modernicons.h").read_text(encoding="cp1252")
    actions = (MODERN / "actions.cpp").read_text(encoding="cp1252")
    whisper = (MODERN / "whisprbx.cpp").read_text(encoding="cp1252")

    resource_files = {
        match.replace("\\\\", "\\").lower()
        for match in re.findall(r'"(res\\[^"\r\n]+\.(?:ico|bmp|dib))"', rc, re.IGNORECASE)
    }
    require(resource_files, "chat.rc contains no audited icon/bitmap resources")
    makefile_lower = makefile.lower()
    for resource_file in sorted(resource_files):
        require(resource_file in makefile_lower,
                f"chat.res does not rebuild when {resource_file} changes")
    require(re.search(
        r'^"\$\(INTDIR\)\\chat\.res"\s*:\s*\$\(RESOURCE_INPUTS\)\s*$',
        makefile, re.MULTILINE) is not None,
        "chat.res does not depend on the complete resource input set")
    for script_input in ("chat.rc", "resource.h", "cchat.rcv", "chatver.rc",
                         "chatver.h", "res\\chat.rc2", "$(ARTINC)\\textview.rc",
                         "$(ARTINC)\\tvres.h", "$(ARTINC)\\hand.cur"):
        require(script_input.lower() in makefile_lower,
                f"chat.res omits included resource input {script_input}")

    require("ApplyDpiAwareWindowIcons" in header,
            "DPI-aware icon helper is not part of the Windows UI contract")
    for token in ("GetSystemMetricsForDpi", "SM_CXICON", "SM_CYICON",
                  "SM_CXSMICON", "SM_CYSMICON", "LR_SHARED"):
        require(token in loader, f"DPI-aware icon loader omits {token}")
    require("window.SetIcon(big_icon, TRUE);" in loader and
            "window.SetIcon(small_icon, FALSE);" in loader,
            "large and small icon frames are not installed independently")
    require("DestroyIcon" not in loader,
            "LR_SHARED icon handles must not be destroyed by application code")

    require("ApplyDpiAwareWindowIcons(*pNotifBox, IDI_NOTIF)" in actions,
            "notification window bypasses the DPI-aware icon helper")
    require("ApplyDpiAwareWindowIcons(*wbox, IDI_WHISPER)" in whisper,
            "whisper window bypasses the DPI-aware icon helper")
    require("theApp.LoadIcon(IDI_NOTIF)" not in actions and
            "theApp.LoadIcon(IDI_WHISPER)" not in whisper,
            "a single default-size HICON is still assigned as both window icons")

    print(f"Windows icon integration passed: {len(resource_files)} resource files, "
          "distinct DPI-aware big/small shared handles")


if __name__ == "__main__":
    main()
