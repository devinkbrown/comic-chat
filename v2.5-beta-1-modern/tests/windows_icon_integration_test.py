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
                  "ImageList_Add", "SetBkColor(CLR_NONE)", "RPC_E_CHANGED_MODE"):
        require(token in loader, f"WIC alpha-strip path omits {token}")
    require("source_width != static_cast<std::uint64_t>(source_height) * image_count" in loader,
            "modern horizontal strip does not enforce an exact square-cell count")
    require("source_height != static_cast<UINT>(declared_source_cell_size)" in loader,
            "modern strip does not validate the size declared by its resource ID")
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

    print(f"Windows icon integration passed: {len(resource_files)} resource files, "
          f"{len(expected_bindings)} x {len(modern_sizes)} WIC alpha-strip resources, "
          "distinct DPI-aware window icons")


if __name__ == "__main__":
    main()
