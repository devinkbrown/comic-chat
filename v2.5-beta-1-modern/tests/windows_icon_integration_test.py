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
    resources = (MODERN / "resource.h").read_text(encoding="cp1252")
    actions = (MODERN / "actions.cpp").read_text(encoding="cp1252")
    whisper = (MODERN / "whisprbx.cpp").read_text(encoding="cp1252")
    notifications = (MODERN / "notipage.cpp").read_text(encoding="cp1252")
    automation = (MODERN / "autopage.cpp").read_text(encoding="cp1252")
    automation_header = (MODERN / "autopage.h").read_text(encoding="cp1252")

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

    generated_root = r"..\\portable\\assets\\icons\\generated\\windows\\"
    generated_icons = {
        "IDR_MAINFRAME": "chat.ico", "IDI_CHAT_DOC": "chatdoc.ico",
        "IDI_CHAT_ROOM": "room.ico", "IDI_RULESET": "ruleset.ico",
        "IDI_AVATAR": "avatar.ico", "IDI_BACKGROUND": "backgd.ico",
        "IDI_RATINGS": "ratings.ico", "IDI_WHISPER": "whisper.ico",
        "IDI_NOTIF": "notif.ico", "IDI_CONNECT_SRV": "tosrv.ico",
        "IDI_CONNECT_NET": "tonet.ico",
    }
    for resource, filename in generated_icons.items():
        require(re.search(
            rf'^{resource}\s+ICON\s+[^\r\n]*"{re.escape(generated_root + filename)}"$',
            rc, re.MULTILINE) is not None,
            f"{resource} is not bound to generated windows/{filename}")
    rc_include = r'#include "..\portable\assets\icons\generated\windows\modern-icon-assets.rcinc"'
    require(rc_include in rc, "chat.rc does not include generated alpha RCDATA")
    for token in ("MODERN_ICON_MAKINC=", "MODERN_ICON_RCINC=",
                  "!IF !EXIST(\"$(MODERN_ICON_MAKINC)\")",
                  "!IF !EXIST(\"$(MODERN_ICON_RCINC)\")",
                  "!INCLUDE \"$(MODERN_ICON_MAKINC)\"",
                  "$(MODERN_ICON_RESOURCE_INPUTS)"):
        require(token.lower() in makefile_lower,
                f"NMAKE generated-resource contract omits {token}")
    require(makefile_lower.count("!error missing generated modern icon") == 2,
            "NMAKE generated-resource absence is not fail-fast")

    require("ApplyDpiAwareWindowIcons" in header,
            "DPI-aware icon helper is not part of the Windows UI contract")
    for token in ("GetSystemMetricsForDpi", "SM_CXICON", "SM_CYICON",
                  "SM_CXSMICON", "SM_CYSMICON", "LoadIconWithScaleDown",
                  "DpiAwareWindowIcons", "DestroyOwnedIconPair"):
        require(token in loader, f"DPI-aware icon loader omits {token}")
    require("LR_SHARED" not in loader and "LoadSharedIconFrame" not in loader,
            "nonstandard per-DPI icon frames can still alias USER32's shared cache")
    require("window.SetIcon(big_icon, TRUE);" in loader and
            "window.SetIcon(small_icon, FALSE);" in loader,
            "large and small icon frames are not installed independently")
    require("ReleaseDpiAwareWindowIcons" in loader and
            "window.SetIcon(nullptr, TRUE);" in loader and
            "window.SetIcon(nullptr, FALSE);" in loader and
            "DestroyIcon" in loader,
            "owned window icons are not detached and destroyed safely")

    require("*pNotifBox, IDI_NOTIF, pNotifBox->m_windowIcons" in actions,
            "notification window bypasses the DPI-aware icon helper")
    require("*wbox, IDI_WHISPER, wbox->m_windowIcons" in whisper,
            "whisper window bypasses the DPI-aware icon helper")
    require("ReleaseDpiAwareWindowIcons(*this, m_windowIcons);" in notifications and
            "ReleaseDpiAwareWindowIcons(*this, m_windowIcons);" in whisper,
            "dialog teardown can destroy an HICON while USER32 still references it")
    require("theApp.LoadIcon(IDI_NOTIF)" not in actions and
            "theApp.LoadIcon(IDI_WHISPER)" not in whisper,
            "a single default-size HICON is still assigned as both window icons")

    modern_families = {
        "TOOLBAR": 4000,
        "TABS": 4010,
        "SAY": 4020,
        "MEMBER": 4030,
        "TEXT": 4040,
        "USER": 4050,
        "CONNECT": 4060,
        "OLDNEW": 4070,
        "INACTIVE": 4080,
        "ACTIVE": 4090,
        "STOPPED": 4100,
    }
    modern_sizes = (16, 20, 24, 32, 40, 48)
    for family, base in modern_families.items():
        for offset, size in enumerate(modern_sizes):
            name = f"IDR_MODERN_PNG_{family}_{size}"
            require(re.search(
                rf"^#define\s+{name}\s+{base + offset}$", resources, re.MULTILINE) is not None,
                f"modern PNG resource ID drifted: {name}")

    expression_sizes = ((20, 26), (25, 33), (30, 39), (40, 52),
                        (50, 65), (60, 78), (80, 104))
    expression_order = ("HAPPY", "COY", "BORED", "SCARED", "SAD",
                        "ANGRY", "SHOUT", "LAUGH")
    for size_index, (width, height) in enumerate(expression_sizes):
        for emotion_index, emotion in enumerate(expression_order):
            name = f"IDR_MODERN_PNG_EXPR_{emotion}_{width}X{height}"
            expected = 4200 + size_index * 10 + emotion_index
            require(re.search(
                rf"^#define\s+{name}\s+{expected}$", resources, re.MULTILINE) is not None,
                f"modern expression resource ID drifted: {name}")

    numeric_defines = [(name, int(value)) for name, value in re.findall(
        r"^#define\s+(\w+)\s+(\d+)\s*$", resources, re.MULTILINE)]
    modern_defines = [(name, value) for name, value in numeric_defines
                      if name.startswith("IDR_MODERN_PNG_")]
    modern_ids = {value for _, value in modern_defines}
    require(len(modern_defines) == 122 and len(modern_ids) == 122,
            "generated PNG IDs are missing or collide with each other")
    require(not [(name, value) for name, value in numeric_defines
                 if 4000 <= value <= 4267 and not name.startswith("IDR_MODERN_PNG_")],
            "generated PNG range collides with a legacy resource/control ID")

    expected_bindings = {
        "IDR_MAINFRAME": ("IDR_MODERN_PNG_TOOLBAR_16", 10),
        "IDB_TABS": ("IDR_MODERN_PNG_TABS_16", 4),
        "IDB_SAY_BAR": ("IDR_MODERN_PNG_SAY_16", 7),
        "IDB_MEMBER": ("IDR_MODERN_PNG_MEMBER_16", 5),
        "IDR_TEXTTOOLBAR": ("IDR_MODERN_PNG_TEXT_16", 7),
        "IDR_USERTOOLBAR": ("IDR_MODERN_PNG_USER_16", 7),
        "IDB_CONNECT": ("IDR_MODERN_PNG_CONNECT_16", 2),
        "IDB_OLDNEW": ("IDR_MODERN_PNG_OLDNEW_16", 1),
        "IDB_INACTIVE": ("IDR_MODERN_PNG_INACTIVE_16", 1),
        "IDB_ACTIVE": ("IDR_MODERN_PNG_ACTIVE_16", 1),
        "IDB_STOPPED": ("IDR_MODERN_PNG_STOPPED_16", 1),
    }
    for legacy, (modern, count) in expected_bindings.items():
        require(re.search(
            rf"\{{{legacy},\s*{modern},\s*{count}\}}", loader) is not None,
            f"missing modern strip binding {legacy} -> {modern} ({count} cells)")
    for token in ("IWICImagingFactory", "WICBitmapInterpolationModeFant",
                  "GUID_WICPixelFormat32bppPBGRA", "CreateDIBSection", "ILC_COLOR32",
                  "ImageList_Add", "SetBkColor(CLR_NONE)", "RPC_E_CHANGED_MODE",
                  "GdiFlush"):
        require(token in loader, f"WIC alpha-strip path omits {token}")
    dib_install = loader[loader.index("bool InstallAlphaImageList("):
                         loader.index("bool BuildPngStripImageList(")]
    require(dib_install.index("std::memcpy(bitmap_bits") <
            dib_install.index("GdiFlush()") <
            dib_install.index("ImageList_Add"),
            "DIBSection writes are not flushed before the image-list GDI consumer")
    require("expected_width" in loader and
            "static_cast<std::uint64_t>(declared_source_cell_size) * image_count" in loader and
            "source_width == expected_width && source_height == expected_height" in loader,
            "modern horizontal strip does not enforce its exact declared cell geometry")
    require("kModernStripSizes[] = {16, 20, 24, 32, 40, 48}" in loader and
            "distance == best_distance" in loader,
            "modern strip selection lost exact/nearest optical-size preference")
    require("ReplaceImageList(image_list, replacement)" in loader and
            "HIMAGELIST previous_handle = target.Detach();" in loader,
            "modern/fallback image-list installation is not atomic")
    require(loader.index("if (BuildPngStripImageList(") <
            loader.index("source.LoadBitmap(legacy_resource)"),
            "legacy bitmap is selected before mapped modern RCDATA")
    require("windowscodecs.lib" in makefile_lower,
            "Windows build does not link the inbox WIC codec library")

    # Expressions use all eight generated PNGs in Microsoft's canonical bodycam
    # order. A failed set must leave no stale list, selecting the original DIBs.
    first_expression_block = re.search(
        r"\{20, 26, \{([^}]+)\}\}", loader)
    require(first_expression_block is not None,
            "20x26 expression resource block is missing")
    offsets = [first_expression_block.group(1).index(
        f"IDR_MODERN_PNG_EXPR_{emotion}_20X26") for emotion in expression_order]
    require(offsets == sorted(offsets), "expression index order no longer matches Microsoft")
    for token in ("FindExpressionSize(face_size)", "BuildModernExpressionImageList",
                  "kExpressionCount", "image_list.DeleteImageList();"):
        require(token in loader, f"atomic expression path omits {token}")

    # Direct notification/rule/status consumers must use alpha-capable loaders,
    # not construct partial masked lists or depend on contiguous bitmap IDs.
    require(notifications.count("BuildOrderedImageList(") == 1 and
            notifications.count("BuildStripImageList(") == 2,
            "notification status/connect/old-new lists bypass modern loaders")
    require("{IDB_INACTIVE, IDB_ACTIVE}" in notifications,
            "notification inactive/active indices drifted")
    require("pItem->iImage = (pNotif->bActive() ? 1 : 0);" in notifications and
            "pItem->iImage = (pUser->GetFlags() & g_wConnected) ? 0 : 1;" in notifications and
            "INDEXTOSTATEIMAGEMASK((pUser->GetFlags() & g_wNew) ? 1 : 0)" in notifications,
            "notification active/connect/new state consumers drifted from source indices")
    require(automation.count("BuildOrderedImageList(") == 2 and
            "{IDB_INACTIVE, IDB_ACTIVE, IDB_STOPPED}" in automation and
            "{IDB_INACTIVE, IDB_ACTIVE}" in automation,
            "automation status lists lost their explicit state order")
    require("pRule->bActive() ? (pRule->bStopped() ? 2 : 1) : 0" in automation,
            "rule inactive/active/stopped consumer drifted from source indices")
    for forbidden in ("m_ilActiveStatus.Create(IDB_INACTIVE", "IDB_INACTIVE+nCnt"):
        require(forbidden not in notifications + automation,
                f"direct masked/status bitmap path remains: {forbidden}")
    require("CImageList\tm_icons;" in automation_header and
            "ImageList_Draw" in automation,
            "owner-drawn rule-set status still uses raw HBITMAP BitBlt")
    require("BuildModernOrderedImageList" in loader and
            "BuildLegacyOrderedImageList" in loader and
            loader.index("BuildModernOrderedImageList(", loader.index("bool BuildOrderedImageList(")) <
            loader.index("BuildLegacyOrderedImageList(", loader.index("bool BuildOrderedImageList(")),
            "ordered state lists do not use atomic modern-to-source fallback")

    print(f"Windows icon integration passed: {len(resource_files)} source fallback files, "
          f"{len(generated_icons)} generated ICO bindings, "
          f"{len(expected_bindings)} x {len(modern_sizes)} WIC alpha-strip resources, "
          "56 expression resources, direct state consumers, distinct DPI-aware window icons")


if __name__ == "__main__":
    main()
