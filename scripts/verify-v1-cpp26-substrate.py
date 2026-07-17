#!/usr/bin/env python3
"""Dependency-free structural proof for the v1 native build substrate."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAKEFILE = ROOT / "v1.0-pre-modern" / "chat.mak"
INVARIANT = ROOT / "v1.0-pre-modern" / "cpp26mode.h"
CONTRACT = (
    ROOT / "v1.0-pre-modern" / "tests" / "transport_adapter_api_compile.cpp"
)
ADAPTER = ROOT / "v1.0-pre-modern" / "transportadapter.h"
MESON = ROOT / "portable" / "meson.build"


class Verification:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def finish(self) -> int:
        if self.failures:
            for failure in self.failures:
                print(f"FAIL: {failure}", file=sys.stderr)
            return 1
        print("v1 C++26/shared-transport substrate verification passed")
        return 0


def logical_assignments(text: str, name: str) -> list[str]:
    """Return NMAKE macro assignments with backslash continuations joined."""
    lines = text.splitlines()
    assignments: list[str] = []
    index = 0
    prefix = f"{name}="
    while index < len(lines):
        line = lines[index]
        if not line.startswith(prefix):
            index += 1
            continue
        parts = [line]
        while parts[-1].rstrip().endswith("\\") and index + 1 < len(lines):
            index += 1
            parts.append(lines[index])
        assignments.append("\n".join(parts))
        index += 1
    return assignments


def main() -> int:
    check = Verification()
    for path in (MAKEFILE, INVARIANT, CONTRACT, ADAPTER, MESON):
        check.require(path.is_file(), f"required file is missing: {path.relative_to(ROOT)}")
    if check.failures:
        return check.finish()

    makefile = MAKEFILE.read_text(encoding="utf-8")
    invariant = INVARIANT.read_text(encoding="utf-8")
    contract = CONTRACT.read_text(encoding="utf-8")
    adapter = ADAPTER.read_text(encoding="utf-8")
    meson = MESON.read_text(encoding="utf-8")
    active_makefile = "\n".join(
        line for line in makefile.splitlines() if not line.lstrip().startswith("#")
    )

    strict_flags = (
        "/std:c++latest",
        "/permissive-",
        "/EHsc",
        "/Zc:__cplusplus",
        "/Zc:preprocessor",
        "/Zc:forScope",
        "/Zc:strictStrings",
        "/Zc:wchar_t",
        "/Zc:inline",
        "/Zc:externConstexpr",
        "/Zc:lambda",
        "/Zc:twoPhase",
        "/Zc:throwingNew",
        "/Zc:ternary",
        "/volatile:iso",
    )
    cpp26 = logical_assignments(makefile, "CPP26_FLAGS")
    check.require(len(cpp26) == 1, "CPP26_FLAGS must have one authoritative definition")
    if cpp26:
        for flag in strict_flags:
            check.require(flag in cpp26[0], f"CPP26_FLAGS is missing {flag}")

    cpp_common = logical_assignments(makefile, "CPP26_COMMON")
    check.require(len(cpp_common) == 1, "CPP26_COMMON must have one definition")
    if cpp_common:
        check.require(
            '/FI"cpp26mode.h"' in cpp_common[0],
            "every C++ recipe must force-include cpp26mode.h",
        )

    cpp_projects = logical_assignments(makefile, "CPP_PROJ")
    c_projects = logical_assignments(makefile, "C_PROJ")
    check.require(len(cpp_projects) == 2, "release and debug must each define CPP_PROJ")
    check.require(len(c_projects) == 2, "release and debug must each define C_PROJ")
    for index, project in enumerate(cpp_projects, start=1):
        check.require("$(CPP26_COMMON)" in project, f"CPP_PROJ #{index} bypasses C++26 common flags")
        check.require("/W4" in project, f"CPP_PROJ #{index} must use /W4")
        for include_path in (
            r"..\portable\include",
            r"..\third_party\libuv\include",
            r"..\third_party\mbedtls\include",
        ):
            check.require(include_path in project, f"CPP_PROJ #{index} omits {include_path}")
    for index, project in enumerate(c_projects, start=1):
        check.require("$(C_COMMON)" in project, f"C_PROJ #{index} bypasses C-only common flags")
        check.require("CPP26" not in project, f"C_PROJ #{index} must not inherit C++26 flags")

    recipe_lines = [
        line.strip()
        for line in makefile.splitlines()
        if "$(CPP)" in line and not line.lstrip().startswith("#")
    ]
    check.require(bool(recipe_lines), "no compiler recipes were found")
    for line in recipe_lines:
        check.require(
            "$(CPP_PROJ)" in line or "$(C_PROJ)" in line,
            f"compiler recipe bypasses the audited C/C++ flag sets: {line}",
        )

    inference_rules = {
        ".c{$(CPP_OBJS)}.obj:": "$(CPP) $(C_PROJ) $<",
        ".cpp{$(CPP_OBJS)}.obj:": "$(CPP) $(CPP_PROJ) $<",
        ".cxx{$(CPP_OBJS)}.obj:": "$(CPP) $(CPP_PROJ) $<",
    }
    for rule, recipe in inference_rules.items():
        pattern = re.escape(rule) + r"\s+" + re.escape(recipe)
        check.require(
            re.search(pattern, makefile) is not None,
            f"{rule} must compile through {'C_PROJ' if '.c{' in rule else 'CPP_PROJ'}",
        )

    shared_objects = {
        "crypto_runtime.obj": r"..\portable\src\crypto_runtime.cpp",
        "memory.obj": r"..\portable\src\memory.cpp",
        "connection_engine.obj": r"..\portable\src\net\connection_engine.cpp",
        "dcc_transfer_engine.obj": r"..\portable\src\net\dcc_transfer_engine.cpp",
        "ircv3.obj": r"..\portable\src\net\ircv3.cpp",
        "sts_policy_store.obj": r"..\portable\src\net\sts_policy_store.cpp",
        "transport_adapter_api_compile.obj": r"tests\transport_adapter_api_compile.cpp",
    }
    link_manifests = logical_assignments(makefile, "LINK32_OBJS")
    check.require(len(link_manifests) == 2, "release and debug must each define LINK32_OBJS")
    for config_index, manifest in enumerate(link_manifests, start=1):
        for object_name in shared_objects:
            check.require(
                f"\\{object_name}" in manifest,
                f"LINK32_OBJS #{config_index} omits {object_name}",
            )
        check.require("\\chat.res" in manifest, f"LINK32_OBJS #{config_index} omits resources")
        check.require("tlssock.obj" not in manifest.lower(), "SChannel tlssock must remain dormant")
    for object_name, source in shared_objects.items():
        rule_pattern = (
            re.escape(f'"$(INTDIR)\\{object_name}"')
            + r"\s*:\s*"
            + re.escape(source)
            + r"\s+cpp26mode\.h"
        )
        check.require(
            re.search(rule_pattern, makefile) is not None,
            f"missing explicit {object_name} compile rule for {source}",
        )

    link_flags = logical_assignments(makefile, "LINK32_FLAGS")
    check.require(len(link_flags) == 2, "release and debug must each define LINK32_FLAGS")
    native_libraries = (
        "libuv.lib",
        "mbedtls.lib",
        "mbedx509.lib",
        "mbedcrypto.lib",
        "bcrypt.lib",
        "crypt32.lib",
        "userenv.lib",
        "iphlpapi.lib",
        "psapi.lib",
        "advapi32.lib",
        "ws2_32.lib",
    )
    for config_index, flags in enumerate(link_flags, start=1):
        for library in native_libraries:
            check.require(library in flags, f"LINK32_FLAGS #{config_index} omits {library}")
        check.require('/machine:I386' in flags, f"LINK32_FLAGS #{config_index} is not x86")
        check.require('$(PORTABLE_LIB)' in flags, f"LINK32_FLAGS #{config_index} omits pinned lib path")
        check.require("secur32.lib" not in flags.lower(), "SChannel must not be activated")

    check.require(
        'rmdir /s /q ".\\Release"' in makefile
        and 'rmdir /s /q ".\\Debug"' in makefile,
        "CLEAN must use fixed Release/Debug directories",
    )
    check.require(
        'rmdir /s /q "$(INTDIR)"' not in makefile
        and 'rmdir /s /q "$(OUTDIR)"' not in makefile,
        "CLEAN must not recursively delete command-line-overridable paths",
    )
    check.require('/out:"$(OUTDIR)/chat.exe"' in makefile, "v1 package output must remain chat.exe")
    check.require("/GX" not in active_makefile, "deprecated /GX remains active")
    check.require("/Gm" not in active_makefile, "deprecated /Gm remains active")
    check.require("/Yu" not in active_makefile, "legacy PCH consumption remains active")

    invariant_tokens = (
        "_MSC_VER < 1951",
        "_MSVC_LANG <= 202302L",
        "__cplusplus == _MSVC_LANG",
        "_MSVC_TRADITIONAL",
    )
    for token in invariant_tokens:
        check.require(token in invariant, f"cpp26mode.h omits invariant {token}")

    contract_headers = (
        '"comicchat/crypto_runtime.hpp"',
        '"comicchat/net/connection_engine.hpp"',
        '"comicchat/net/dcc_transfer_engine.hpp"',
        '"comicchat/net/ircv3.hpp"',
        '"comicchat/net/sts_policy_store.hpp"',
    )
    for header in contract_headers:
        check.require(header in contract, f"adapter compile contract omits {header}")
    check.require("CAsyncSocket" not in contract, "compile contract must remain MFC-independent")
    check.require("CAsyncSocket" not in adapter, "v1 adapter policy must remain MFC-independent")
    for token in (
        "class SessionGate final",
        "class WakeupGate final",
        "PrepareOutbound(",
        "maximum_irc_wire_bytes",
    ):
        check.require(token in adapter, f"v1 adapter policy omits {token}")
    check.require(
        '"../transportadapter.h"' in contract,
        "adapter compile contract does not compile the live v1 policy seam",
    )
    check.require(
        "../v1.0-pre-modern/tests/transport_adapter_api_compile.cpp" in meson,
        "portable Clang build does not compile the v1 adapter API contract",
    )
    check.require(
        "../v1.0-pre-modern/tests/transport_adapter_test.cpp" in meson,
        "portable Clang build does not run the v1 adapter causal tests",
    )

    return check.finish()


if __name__ == "__main__":
    raise SystemExit(main())
