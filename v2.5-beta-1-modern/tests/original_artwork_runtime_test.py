#!/usr/bin/env python3
"""Prove Microsoft artwork remains the safe fallback during icon staging."""

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

    require("FindResourceW" in loader and "BuildPngStripImageList" in loader,
            "modern alpha-strip probe is missing")
    require("source.LoadBitmap(legacy_resource)" in loader,
            "strip loader lost the original RT_BITMAP fallback")
    require(loader.index("if (BuildPngStripImageList(") <
            loader.index("source.LoadBitmap(legacy_resource)"),
            "legacy bitmap is selected before the modern alpha-strip probe")
    require("COLORONCOLOR" in loader,
            "source-palette scaling no longer preserves original pixel relationships")

    face_start = loader.find("bool BuildExpressionImageList(")
    require(face_start >= 0, "cannot audit bodycam artwork selector")
    face_body = loader[face_start:]
    require("BuildModernExpressionImageList(image_list, *binding)" in face_body,
            "bodycam does not probe the complete generated expression set")
    require(face_body.index("BuildModernExpressionImageList(image_list, *binding)") <
            face_body.index("image_list.DeleteImageList();") <
            face_body.index("return false;"),
            "failed expression probes can retain a stale generated list")
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

    user_toolbar = re.search(
        r"IDR_USERTOOLBAR TOOLBAR[^\r\n]*[\s\S]+?\nEND", rc)
    require(user_toolbar is not None, "cannot audit member toolbar commands")
    require("ID_SEND_FILE" not in user_toolbar.group(0),
            "Microsoft's NetMeeting glyph is falsely exposed as Send File")

    print("original artwork fallback audit passed: 11 ICOs, 6 bitmap families, "
          "atomic bodycam CDIB fallback; modern probes precede source fallback")


if __name__ == "__main__":
    main()
