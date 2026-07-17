#!/usr/bin/env python3
"""Audit the standalone native Windows UI test and cleanup contract."""

from __future__ import annotations

from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
MODERN = REPOSITORY / "v2.5-beta-1-modern"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def link_recipe(makefile: str, output: str) -> str:
    target = f'"$(OUTDIR)\\{output}" :'
    start = makefile.find(target)
    require(start >= 0, f"NMAKE target is missing for {output}")
    link = makefile.find("$(LINK32) @<<", start)
    end = makefile.find("\n<<", link)
    require(link >= 0 and end >= 0, f"NMAKE link recipe is malformed for {output}")
    return makefile[link:end]


def main() -> None:
    makefile = (MODERN / "chat.mak").read_text(encoding="cp1252")
    workflow = (REPOSITORY / ".github/workflows/build-modern.yml").read_text(
        encoding="utf-8"
    )
    meson = (REPOSITORY / "portable/meson.build").read_text(encoding="utf-8")
    ircsock = (MODERN / "ircsock.cpp").read_text(encoding="cp1252")

    tests_target = (
        'TESTS : "$(OUTDIR)\\modernui_test.exe" '
        '"$(OUTDIR)\\transport_ui_bridge_test.exe" '
        '"$(OUTDIR)\\sts_session_test.exe"'
    )
    require(tests_target in makefile, "NMAKE TESTS does not own every standalone output")

    modernui_link = link_recipe(makefile, "modernui_test.exe")
    for required in (
        "$(TEST_LINK32_FLAGS)",
        '"$(INTDIR)\\modernui_test.obj"',
        '"$(INTDIR)\\modernui.obj"',
    ):
        require(required in modernui_link, f"modernui test link omits {required}")
    for forbidden in ("$(OBJS)", "chat.obj", "chat.res", "modernicons.obj"):
        require(forbidden not in modernui_link,
                f"modernui test link imports application object {forbidden}")

    bridge_link = link_recipe(makefile, "transport_ui_bridge_test.exe")
    require('"$(INTDIR)\\transport_ui_bridge_test.obj"' in bridge_link,
            "transport bridge test link omits its test object")
    for forbidden in ("$(OBJS)", "chat.obj", "chat.res", "modernui.obj"):
        require(forbidden not in bridge_link,
                f"transport bridge test link imports unrelated object {forbidden}")

    sts_link = link_recipe(makefile, "sts_session_test.exe")
    for required in (
        '"$(INTDIR)\\sts_session_test.obj"',
        '"$(INTDIR)\\private_config.obj"',
        '"$(INTDIR)\\sts_policy_store.obj"',
        '"$(INTDIR)\\sts_session.obj"',
    ):
        require(required in sts_link, f"STS session test link omits {required}")
    for forbidden in ("$(OBJS)", "chat.obj", "chat.res", "modernui.obj"):
        require(forbidden not in sts_link,
                f"STS session test link imports unrelated object {forbidden}")

    flags_start = makefile.index("TEST_LINK32_FLAGS=")
    flags_end = makefile.index("\n\n", flags_start)
    flags = makefile[flags_start:flags_end].lower()
    for token in (
        "/subsystem:console",
        "user32.lib",
        "gdi32.lib",
        "advapi32.lib",
        "comctl32.lib",
        "ole32.lib",
        "shell32.lib",
    ):
        require(token in flags, f"standalone UI test link flags omit {token}")
    require("/nodefaultlib" not in flags,
            "standalone tests suppress the CRT or default Win32 import libraries")

    require('CLEAN :' in makefile, "NMAKE CLEAN target is missing")
    require('if exist ".\\Release\\." rmdir /s /q ".\\Release"' in makefile,
            "Release CLEAN does not remove the complete configuration directory")
    require('if exist ".\\Debug\\." rmdir /s /q ".\\Debug"' in makefile,
            "Debug CLEAN does not remove the complete configuration directory")
    require('rmdir /s /q "$(OUTDIR)"' not in makefile,
            "CLEAN recursively deletes a caller-overridable path")

    workflow_order = (
        'nmake /f chat.mak CFG="chat - Win32 Release" TESTS',
        'Release\\modernui_test.exe',
        'Release\\transport_ui_bridge_test.exe',
        'Release\\sts_session_test.exe',
    )
    cursor = 0
    for command in workflow_order:
        cursor = workflow.find(command, cursor)
        require(cursor >= 0, f"Windows CI does not execute {command}")
        cursor += len(command)
        error_check = workflow.find("if errorlevel 1 exit /b 1", cursor)
        require(error_check >= 0, f"Windows CI ignores failure from {command}")
        cursor = error_check + 1

    platform_guard = meson.find("if host_machine.system() != 'windows'")
    modernui_target = meson.find("modernui_tests = executable(", platform_guard)
    platform_end = meson.find("\n  endif", modernui_target)
    bridge_target = meson.find("transport_ui_bridge_tests = executable(", platform_end)
    require(0 <= platform_guard < modernui_target < platform_end < bridge_target,
            "Meson platform guard does not isolate only the MFC-backed modern UI source")
    require("test('comicchat-modernui', modernui_tests" in
            meson[modernui_target:platform_end],
            "Meson does not register the guarded portable modern UI test")
    require("test('comicchat-transport-ui-bridge', transport_ui_bridge_tests" in
            meson[bridge_target:],
            "Meson does not register the portable transport UI bridge test")

    ensure_start = ircsock.index("BOOL CIrcSocket::EnsureStsPolicyLoaded()")
    ensure_end = ircsock.index("BOOL CIrcSocket::FinishStsTransport", ensure_start)
    ensure_sts = ircsock[ensure_start:ensure_end]
    existing_owner = ensure_sts.index("if (m_stsSession) {")
    replacement = ensure_sts.index("m_stsSession.emplace(*path);")
    require(existing_owner < replacement,
            "STS setup does not inspect an existing coordinator before replacement")
    require("return FALSE;" in ensure_sts[existing_owner:replacement],
            "an unhealthy STS coordinator can be replaced and clear its fail-closed latch")

    close_start = ircsock.index("void CIrcSocket::Close()")
    close_end = ircsock.index("BOOL CIrcSocket::IsOpen()", close_start)
    close_sts = ircsock[close_start:close_end]
    require("const BOOL stsFinished = FinishStsTransport" in close_sts and
            "if (!stsFinished)" in close_sts,
            "Close ignores durable STS disconnect/rebase failure")

    print("Windows native standalone test gate audit passed")


if __name__ == "__main__":
    main()
