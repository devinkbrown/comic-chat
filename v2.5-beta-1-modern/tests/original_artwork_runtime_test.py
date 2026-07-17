#!/usr/bin/env python3
"""Prove rejected generated artwork is unreachable from the MFC runtime."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODERN = ROOT / "v2.5-beta-1-modern"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    rc = (MODERN / "chat.rc").read_text(encoding="cp1252")
    loader = (MODERN / "modernicons.cpp").read_text(encoding="cp1252")
    bodycam = (MODERN / "bodycam.cpp").read_text(encoding="cp1252")

    require("modern-ui-assets" not in rc, "chat.rc still embeds generated artwork")
    require("RT_RCDATA" not in loader, "icon loader can still resolve generated RCDATA")
    require("FindResource" not in loader and "DecodePngResource" not in loader,
            "icon loader retains a generated PNG resource path")
    require("kResourceBase" not in loader and "4000" not in loader and "5000" not in loader,
            "generated resource ID ranges remain addressable")
    require("source.LoadBitmap(legacy_resource)" in loader,
            "strip loader is not rooted in the original RT_BITMAP resource")
    require("COLORONCOLOR" in loader,
            "source-palette scaling no longer preserves original pixel relationships")

    face_body = re.search(
        r"bool BuildExpressionImageList\([^}]+?\n\}", loader, re.DOTALL)
    require(face_body is not None, "cannot audit bodycam artwork selector")
    require("image_list.DeleteImageList();" in face_body.group(0) and
            "return false;" in face_body.group(0),
            "bodycam can retain or select a generated expression image list")
    require("else\n\t\t\tIcons.GetIcon (i)->Draw" in bodycam,
            "bodycam no longer falls back to the original Microsoft CDIBs")

    expected_icons = {
        "IDR_MAINFRAME": "chat.ico", "IDI_CHAT_DOC": "chatdoc.ico",
        "IDI_CHAT_ROOM": "room.ico", "IDI_RULESET": "ruleset.ico",
        "IDI_AVATAR": "avatar.ico", "IDI_BACKGROUND": "backgd.ico",
        "IDI_RATINGS": "ratings.ico", "IDI_WHISPER": "whisper.ico",
        "IDI_NOTIF": "notif.ico", "IDI_CONNECT_SRV": "tosrv.ico",
        "IDI_CONNECT_NET": "tonet.ico",
    }
    for resource, filename in expected_icons.items():
        require(re.search(
            rf"^{resource}\s+ICON\s+[^\r\n]*\"res\\\\{re.escape(filename)}\"$",
            rc, re.MULTILINE) is not None,
            f"{resource} is not bound to original res/{filename}")
        require((MODERN / "res" / filename).is_file(), f"missing original res/{filename}")

    expected_bitmaps = {
        "IDB_SAY_BAR": "balloons.bmp", "IDR_MAINFRAME": "toolbar.bmp",
        "IDB_TABS": "tabbar.bmp", "IDB_MEMBER": "member.bmp",
        "IDR_TEXTTOOLBAR": "texttool.bmp", "IDR_USERTOOLBAR": "usertool.bmp",
    }
    for resource, filename in expected_bitmaps.items():
        require(re.search(
            rf"^{resource}\s+BITMAP\s+[^\r\n]*\"res\\\\{re.escape(filename)}\"$",
            rc, re.MULTILINE) is not None,
            f"{resource} is not bound to original res/{filename}")

    for filename in ("chat.cpp", "chatbars.cpp", "tabbar.cpp", "saywnd.cpp"):
        text = (MODERN / filename).read_text(encoding="cp1252")
        require("BuildStripImageList(" in text,
                f"{filename} bypasses the audited original-bitmap loader")

    require(not (MODERN / "res" / "modern-ui-assets.rcinc").exists(),
            "generated resource include remains in the effective resource tree")
    require(not (MODERN / "res" / "modern-ui").exists(),
            "rejected generated artwork remains in the effective resource tree")
    print("original artwork runtime audit passed: 11 ICOs, 6 bitmap families, "
          "bodycam CDIB fallback; no generated RCDATA path")


if __name__ == "__main__":
    main()
