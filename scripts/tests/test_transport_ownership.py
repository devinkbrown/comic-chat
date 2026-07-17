#!/usr/bin/env python3
"""Causal tests for the transport ownership trust-boundary gate."""

from __future__ import annotations

import importlib.util
import shutil
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
MODULE_PATH = REPOSITORY / "scripts/check-transport-ownership.py"
SPEC = importlib.util.spec_from_file_location("comicchat_transport_ownership", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"could not load {MODULE_PATH}")
gate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gate
SPEC.loader.exec_module(gate)


def write(root: Path, relative: str, contents: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8")


def valid_v2_makefile() -> str:
    object_lines: list[str] = []
    rule_lines: list[str] = []
    for _, object_name, source_name in gate.MAKEFILE_SUBSTRATE_UNITS:
        windows_source = source_name.replace("/", "\\")
        object_lines.append(f'\t"$(INTDIR)\\{object_name}"')
        rule_lines.append(
            f'"$(INTDIR)\\{object_name}" : {windows_source}\n'
            f"\t$(CPP) $(CPP_PROJ) {windows_source}"
        )
    objects = " \\\n".join(object_lines)
    rules = "\n".join(rule_lines)
    includes = " ".join(
        '/I "' + include.replace("/", "\\") + '"'
        for _, include in gate.MAKEFILE_SUBSTRATE_INCLUDES
    )
    libraries = " ".join(library for _, library in gate.MAKEFILE_SUBSTRATE_LIBRARIES)
    return (
        f"CPP_PROJ={includes}\n"
        f"LINK32_FLAGS={libraries}\n"
        + "OBJS= \\\n"
        + objects
        + "\n"
        + rules
        + "\n"
    )


class TransportOwnershipGateTest(unittest.TestCase):
    def test_current_repository_passes_with_exact_deferred_inventory(self) -> None:
        result = gate.audit_repository(REPOSITORY)
        self.assertEqual([], result.errors)
        self.assertEqual(28, len(result.v1_findings))
        self.assertEqual(0, len(result.v1_makefile_deficits))
        allowed_paths = {finding.path for finding in result.allowed_network_findings}
        self.assertEqual(gate.NETWORK_IMPLEMENTATION_ALLOWLIST, allowed_paths)

    def test_count_preserving_v1_send_substitution_and_relocation_fail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in {occurrence[1] for occurrence in gate.V1_EXPECTED_OCCURRENCES}:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(REPOSITORY / relative, destination)

            baseline = gate.scan_v1_legacy_inventory(root)
            self.assertEqual([], gate.compare_v1_inventory(baseline))
            source_path = root / "v1.0-pre-modern/irc.cpp"
            lines = source_path.read_text(encoding="utf-8").splitlines()
            original_lines = list(lines)
            lines[482] = lines[482].replace("outBuff", "replacement", 1)
            source_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            substituted = gate.scan_v1_legacy_inventory(root)
            self.assertEqual(len(baseline), len(substituted), "the attack must preserve the count")
            self.assertNotEqual([], gate.compare_v1_inventory(substituted))

            lines = original_lines
            lines[482] = "\t// exact allowlist test removes the original call"
            lines[483] = "\tserverConn.Send(outBuff, strlen(outBuff));"
            source_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

            relocated = gate.scan_v1_legacy_inventory(root)
            errors = gate.compare_v1_inventory(relocated)
            self.assertEqual(len(baseline), len(relocated), "the attack must preserve the count")
            self.assertTrue(any("occurrence missing" in error for error in errors))
            self.assertTrue(any("unallowlisted occurrence" in error for error in errors))

    def test_new_v1_legacy_socket_in_another_file_is_not_directory_allowlisted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in {occurrence[1] for occurrence in gate.V1_EXPECTED_OCCURRENCES}:
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(REPOSITORY / relative, destination)
            write(
                root,
                "v1.0-pre-modern/alternate.cpp",
                "class AlternateSocket : public CSocket {};\n",
            )
            errors = gate.compare_v1_inventory(gate.scan_v1_legacy_inventory(root))
            self.assertTrue(any("alternate.cpp" in error for error in errors))
            self.assertTrue(any("unallowlisted occurrence" in error for error in errors))

    def test_v2_mfc_socket_regression_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(
                root,
                "v2.5-beta-1-modern/rogue.cpp",
                "#include <afxsock.h>\nclass Rogue : public CAsyncSocket {};\n"
                "void start() { AfxSocketInit(); }\n",
            )
            findings = gate.scan_v2_legacy_ownership(root)
            self.assertEqual(
                {"mfc-socket-header", "mfc-socket-base", "mfc-socket-init"},
                {finding.rule for finding in findings},
            )

    def test_network_apis_are_confined_to_two_shared_engines(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(
                root,
                "portable/src/net/connection_engine.cpp",
                "void io() { ::send(fd, data, size, 0); mbedtls_ssl_read(ssl, data, size); }\n",
            )
            write(
                root,
                "portable/src/net/dcc_transfer_engine.cpp",
                "void dcc() { uv_tcp_init(loop, tcp); }\n",
            )
            write(
                root,
                "portable/src/rogue.cpp",
                "void bypass() { uv_tcp_connect(request, tcp, address, callback); }\n",
            )
            allowed, forbidden = gate.scan_network_ownership(root)
            self.assertEqual(3, len(allowed))
            self.assertEqual(1, len(forbidden))
            self.assertEqual(Path("portable/src/rogue.cpp"), forbidden[0].path)

    def test_comments_literals_disabled_code_and_tests_do_not_spoof_source_scan(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(
                root,
                "portable/src/clean.cpp",
                "// ::send(fd, data, size, 0);\n"
                'const char* mention = "uv_tcp_connect(request, tcp, address, callback)";\n'
                "#if 0\n::recv(fd, data, size, 0);\n#endif\n"
                "auto bytes = 0xffff'ffffULL;\n",
            )
            write(
                root,
                "portable/tests/loopback.cpp",
                "void server() { ::send(fd, data, size, 0); }\n",
            )
            allowed, forbidden = gate.scan_network_ownership(root)
            self.assertEqual([], allowed)
            self.assertEqual([], forbidden)

    def test_makefile_checks_require_active_semantic_assignments_and_rules(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            makefile = valid_v2_makefile()
            errors, deficits = self._check_makefiles(root, makefile)
            self.assertEqual([], errors)
            self.assertEqual(0, len(deficits))

            spoofed = makefile.replace(
                '\t"$(INTDIR)\\connection_engine.obj" \\\n', ""
            ).replace("libuv.lib", "")
            spoofed += (
                "# OBJS=$(INTDIR)\\connection_engine.obj\n"
                "# LINK32_FLAGS=libuv.lib\n"
                "!IF 0\n"
                "OBJS=$(INTDIR)\\connection_engine.obj\n"
                "LINK32_FLAGS=libuv.lib\n"
                "!ENDIF\n"
            )
            errors, _ = self._check_makefiles(root, spoofed)
            self.assertTrue(any("object:connection-engine" in error for error in errors))
            self.assertTrue(any("library:libuv" in error for error in errors))

    def test_partial_v1_makefile_substrate_is_fatal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(root, "v2.5-beta-1-modern/chat.mak", valid_v2_makefile())
            write(
                root,
                "v1.0-pre-modern/chat.mak",
                'LINK32_OBJS="$(INTDIR)\\connection_engine.obj"\n',
            )
            errors, deficits = gate.check_makefiles(root)
            self.assertTrue(any("v1 makefile does not prove" in error for error in errors))
            self.assertEqual(16, len(deficits))

    def test_schannel_experiment_must_remain_unwired(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            write(
                root,
                "v1.0-pre-modern/tlssock.cpp",
                '#include "tlssock.h"\nAcquireCredentialsHandle();\n',
            )
            write(
                root,
                "v1.0-pre-modern/consumer.cpp",
                '#include "tlssock.h"\n#include <schannel.h>\nInitializeSecurityContext();\n',
            )
            findings = gate.scan_schannel_activation(root)
            self.assertEqual(3, len(findings))
            self.assertEqual(
                {"schannel-transport-include", "schannel-transport-api"},
                {finding.rule for finding in findings},
            )

    def _check_makefiles(self, root: Path, v2_makefile: str) -> tuple[list[str], tuple[str, ...]]:
        write(root, "v1.0-pre-modern/chat.mak", valid_v2_makefile())
        write(root, "v2.5-beta-1-modern/chat.mak", v2_makefile)
        return gate.check_makefiles(root)


if __name__ == "__main__":
    unittest.main()
