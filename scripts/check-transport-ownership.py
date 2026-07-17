#!/usr/bin/env python3
"""Enforce exclusive ownership of modern Comic Chat network transport.

Every modern client is a zero-tolerance surface. Legacy-named Connect, Send,
and Close methods may remain as UI/protocol compatibility facades, but MFC
socket ownership, callbacks, handles, initialization, and receive operations
must never return. The shared C++26 transport substrate is mandatory in both
Windows configurations.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Pattern, Sequence


SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"})
PRODUCT_ROOTS = (
    Path("portable/src"),
    Path("v2.5-beta-1-modern"),
)
NETWORK_IMPLEMENTATION_ALLOWLIST = frozenset(
    {
        Path("portable/src/net/connection_engine.cpp"),
        Path("portable/src/net/dcc_transfer_engine.cpp"),
    }
)


@dataclass(frozen=True)
class Finding:
    rule: str
    path: Path
    line: int
    signature: str
    excerpt: str

    def render(self) -> str:
        return f"{self.path.as_posix()}:{self.line}: [{self.rule}] {self.excerpt.strip()}"


@dataclass(frozen=True)
class SourceSnapshot:
    source: str
    searchable: str
    include_searchable: str


@dataclass
class AuditResult:
    errors: list[str]
    allowed_network_findings: list[Finding]

    @property
    def ok(self) -> bool:
        return not self.errors


V2_FORBIDDEN_RULES = (
    (
        "mfc-socket-header",
        re.compile(r"(?m)^[ \t]*#[ \t]*include[ \t]*[<\"]afxsock\.h[>\"]"),
        True,
    ),
    ("mfc-socket-base", re.compile(r"\b(?:CAsyncSocket|CSocket)\b"), False),
    ("mfc-socket-init", re.compile(r"\bAfxSocketInit\s*\("), False),
    ("socket-handle-access", re.compile(r"\bm_hSocket\b"), False),
    ("mfc-receive-callback", re.compile(r"\bOnReceive\s*\("), False),
    ("inherited-create", re.compile(r"\bserverConn\s*\.\s*Create\s*\("), False),
)

NETWORK_API_RULES = (
    (
        "direct-socket-api",
        re.compile(
            r"\b(?:socket|socketpair|connect|send|sendto|sendmsg|recv|recvfrom|recvmsg|"
            r"getsockname|getpeername|getaddrinfo|accept|bind|listen|shutdown|closesocket|"
            r"ioctlsocket|WSAStartup|WSACleanup|WSASocket|WSAConnect|WSASend|WSASendTo|"
            r"WSARecv|WSARecvFrom)\s*\("
        ),
    ),
    (
        "direct-libuv-network-api",
        re.compile(
            r"\b(?:uv_getaddrinfo|uv_freeaddrinfo|uv_fileno|uv_(?:tcp|udp|poll)_[A-Za-z0-9_]+|"
            r"uv_read_start|uv_read_stop|uv_write2?|uv_try_write2?|uv_listen|uv_accept)\s*\("
        ),
    ),
    (
        "direct-mbedtls-transport-api",
        re.compile(
            r"\bmbedtls_ssl_(?:setup|set_hostname|set_bio|handshake|read|write|close_notify)\s*\("
        ),
    ),
)

SCHANNEL_API_PATTERN = re.compile(
    r"\b(?:AcquireCredentialsHandle|InitializeSecurityContext|EncryptMessage|DecryptMessage|"
    r"QueryContextAttributes)\s*\("
)
TLSSOCK_INCLUDE_PATTERN = re.compile(
    r"(?m)^[ \t]*#[ \t]*include[ \t]*[<\"](?:tlssock|schannel|security|sspi)\.h[>\"]"
)

# Every semantic build check is independently inventoried.  That makes partial
# substrate wiring visible instead of hiding it behind one compound bit.
MAKEFILE_SUBSTRATE_UNITS = (
    ("crypto-runtime", "crypto_runtime.obj", "../portable/src/crypto_runtime.cpp"),
    ("connection-engine", "connection_engine.obj", "../portable/src/net/connection_engine.cpp"),
    ("dcc-transfer-engine", "dcc_transfer_engine.obj", "../portable/src/net/dcc_transfer_engine.cpp"),
    ("ircv3", "ircv3.obj", "../portable/src/net/ircv3.cpp"),
    ("sts-policy-store", "sts_policy_store.obj", "../portable/src/net/sts_policy_store.cpp"),
    (
        "adapter-contract",
        "transport_adapter_api_compile.obj",
        "tests/transport_adapter_api_compile.cpp",
    ),
)
MAKEFILE_SUBSTRATE_INCLUDES = (
    ("portable", "../portable/include"),
    ("libuv", "../third_party/libuv/include"),
    ("mbedtls", "../third_party/mbedtls/include"),
)
MAKEFILE_SUBSTRATE_LIBRARIES = (
    ("libuv", "libuv.lib"),
    ("mbedtls", "mbedtls.lib"),
    ("mbedx509", "mbedx509.lib"),
    ("mbedcrypto", "mbedcrypto.lib"),
)
MAKEFILE_SUBSTRATE_CHECKS = (
    *(f"object:{name}" for name, _, _ in MAKEFILE_SUBSTRATE_UNITS),
    *(f"rule:{name}" for name, _, _ in MAKEFILE_SUBSTRATE_UNITS),
    *(f"include:{name}" for name, _ in MAKEFILE_SUBSTRATE_INCLUDES),
    *(f"library:{name}" for name, _ in MAKEFILE_SUBSTRATE_LIBRARIES),
)
FORBIDDEN_MAKEFILE_TRANSPORT_MARKERS = (
    "tlssock.obj",
    "tlssock.cpp",
    "secur32.lib",
    "schannel.lib",
)


def _mask_line(line: str) -> str:
    return "".join("\n" if char == "\n" else "\r" if char == "\r" else " " for char in line)


def mask_literal_zero_branches(text: str) -> str:
    """Mask literal #if 0 branches while conservatively scanning unknown ones."""

    frames: list[tuple[bool, bool]] = []
    disabled = False
    output: list[str] = []
    for line in text.splitlines(keepends=True):
        directive = re.match(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$", line)
        output.append(_mask_line(line) if disabled and directive is None else line)
        if directive is None:
            continue

        keyword, expression = directive.group(1), directive.group(2).strip()
        if keyword in {"if", "ifdef", "ifndef"}:
            literal_zero = keyword == "if" and re.match(r"^(?:0|false)\b", expression) is not None
            frames.append((disabled, literal_zero))
            disabled = disabled or literal_zero
        elif keyword in {"else", "elif"} and frames:
            parent_disabled, literal_zero = frames[-1]
            # For an unknown condition, scan both branches.  For #if 0, scan
            # the else/elif branch because it can be active.
            disabled = parent_disabled if literal_zero else parent_disabled
        elif keyword == "endif" and frames:
            parent_disabled, _ = frames.pop()
            disabled = parent_disabled
    return "".join(output)


def sanitize_cpp(text: str, *, preserve_literals: bool = False) -> str:
    """Mask comments and optionally literals without changing offsets."""

    text = mask_literal_zero_branches(text)
    output = list(text)
    index = 0
    length = len(text)
    state = "code"
    raw_closer = ""

    def mask(position: int) -> None:
        if text[position] not in "\r\n":
            output[position] = " "

    def is_digit_separator(position: int) -> bool:
        if position == 0 or position + 1 >= length or not text[position + 1].isalnum():
            return False
        token_start = position - 1
        while token_start >= 0 and (text[token_start].isalnum() or text[token_start] in "_'"):
            token_start -= 1
        token = text[token_start + 1 : position]
        return bool(token) and token[0].isdigit()

    while index < length:
        if state == "line-comment":
            if text[index] in "\r\n":
                state = "code"
            else:
                mask(index)
            index += 1
            continue
        if state == "block-comment":
            if text.startswith("*/", index):
                mask(index)
                if index + 1 < length:
                    mask(index + 1)
                index += 2
                state = "code"
            else:
                mask(index)
                index += 1
            continue
        if state in {"string", "character"}:
            quote = '"' if state == "string" else "'"
            if not preserve_literals:
                mask(index)
            if text[index] == "\\" and index + 1 < length:
                if not preserve_literals:
                    mask(index + 1)
                index += 2
            elif text[index] == quote:
                index += 1
                state = "code"
            else:
                index += 1
            continue
        if state == "raw-string":
            if text.startswith(raw_closer, index):
                if not preserve_literals:
                    for position in range(index, min(index + len(raw_closer), length)):
                        mask(position)
                index += len(raw_closer)
                state = "code"
            else:
                if not preserve_literals:
                    mask(index)
                index += 1
            continue

        if text.startswith("//", index):
            mask(index)
            mask(index + 1)
            index += 2
            state = "line-comment"
        elif text.startswith("/*", index):
            mask(index)
            mask(index + 1)
            index += 2
            state = "block-comment"
        elif text.startswith('R"', index):
            delimiter_end = text.find("(", index + 2, min(index + 19, length))
            if delimiter_end != -1:
                delimiter = text[index + 2 : delimiter_end]
                raw_closer = ")" + delimiter + '"'
                if not preserve_literals:
                    for position in range(index, delimiter_end + 1):
                        mask(position)
                index = delimiter_end + 1
                state = "raw-string"
            else:
                index += 1
        elif text[index] == '"':
            if not preserve_literals:
                mask(index)
            index += 1
            state = "string"
        elif text[index] == "'" and is_digit_separator(index):
            index += 1
        elif text[index] == "'":
            if not preserve_literals:
                mask(index)
            index += 1
            state = "character"
        else:
            index += 1
    return "".join(output)


def _read_text(path: Path) -> str:
    """Read legacy text while preserving every byte used by ASCII gate tokens."""

    data = path.read_bytes()
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        # Several Microsoft-era translation units use a legacy code page.
        # Latin-1 is a one-byte mapping, so ASCII source tokens and line
        # offsets stay exact without guessing the prose encoding.
        return data.decode("latin-1")


def _find_matches(
    source: str,
    searchable: str,
    relative: Path,
    rule: str,
    pattern: Pattern[str],
) -> list[Finding]:
    lines = source.splitlines()
    searchable_lines = searchable.splitlines()
    findings: list[Finding] = []
    for match in pattern.finditer(searchable):
        line = source.count("\n", 0, match.start()) + 1
        excerpt = lines[line - 1] if line <= len(lines) else ""
        active_line = searchable_lines[line - 1] if line <= len(searchable_lines) else ""
        signature = re.sub(r"\s+", " ", active_line.strip())
        findings.append(
            Finding(rule=rule, path=relative, line=line, signature=signature, excerpt=excerpt)
        )
    return findings


def iter_product_sources(root: Path) -> Iterable[Path]:
    for product_root in PRODUCT_ROOTS:
        absolute_root = root / product_root
        if not absolute_root.is_dir():
            continue
        for path in sorted(absolute_root.rglob("*")):
            if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES and "tests" not in path.parts:
                yield path.relative_to(root)


def load_product_sources(root: Path) -> dict[Path, SourceSnapshot]:
    snapshots: dict[Path, SourceSnapshot] = {}
    for relative in iter_product_sources(root):
        source = _read_text(root / relative)
        snapshots[relative] = SourceSnapshot(
            source=source,
            searchable=sanitize_cpp(source),
            include_searchable=sanitize_cpp(source, preserve_literals=True),
        )
    return snapshots


def scan_v2_legacy_ownership(
    root: Path, snapshots: dict[Path, SourceSnapshot] | None = None
) -> list[Finding]:
    findings: list[Finding] = []
    snapshots = snapshots if snapshots is not None else load_product_sources(root)
    for relative, snapshot in snapshots.items():
        if not relative.is_relative_to("v2.5-beta-1-modern"):
            continue
        for name, pattern, preserve_literals in V2_FORBIDDEN_RULES:
            rule_text = snapshot.include_searchable if preserve_literals else snapshot.searchable
            findings.extend(_find_matches(snapshot.source, rule_text, relative, name, pattern))
    return findings


def scan_network_ownership(
    root: Path, snapshots: dict[Path, SourceSnapshot] | None = None
) -> tuple[list[Finding], list[Finding]]:
    allowed: list[Finding] = []
    forbidden: list[Finding] = []
    snapshots = snapshots if snapshots is not None else load_product_sources(root)
    for relative, snapshot in snapshots.items():
        for name, pattern in NETWORK_API_RULES:
            matches = _find_matches(snapshot.source, snapshot.searchable, relative, name, pattern)
            if relative in NETWORK_IMPLEMENTATION_ALLOWLIST:
                allowed.extend(matches)
            else:
                forbidden.extend(matches)
    return allowed, forbidden


def scan_schannel_activation(
    root: Path, snapshots: dict[Path, SourceSnapshot] | None = None
) -> list[Finding]:
    findings: list[Finding] = []
    snapshots = snapshots if snapshots is not None else load_product_sources(root)
    for relative, snapshot in snapshots.items():
        findings.extend(
            _find_matches(
                snapshot.source,
                snapshot.include_searchable,
                relative,
                "schannel-transport-include",
                TLSSOCK_INCLUDE_PATTERN,
            )
        )
        findings.extend(
            _find_matches(
                snapshot.source,
                snapshot.searchable,
                relative,
                "schannel-transport-api",
                SCHANNEL_API_PATTERN,
            )
        )
    return findings


def _normalize_makefile(text: str) -> str:
    return text.lower().replace("\\", "/")


def _evaluate_nmake_condition(expression: str, configuration: str) -> bool | None:
    expression = re.sub(r"\$\(CFG\)", configuration, expression, flags=re.IGNORECASE).strip()
    if expression in {"0", "false", "FALSE"}:
        return False
    if expression in {"1", "true", "TRUE"}:
        return True
    comparison = re.fullmatch(r'"([^"]*)"\s*(==|!=)\s*"([^"]*)"', expression)
    if comparison is None:
        return None
    left, operator, right = comparison.groups()
    return left == right if operator == "==" else left != right


def active_nmake_text(text: str, configuration: str) -> str:
    """Return active NMAKE lines for one CFG, excluding comments and !IF 0."""

    # parent-active, condition-known, prior-branch-taken
    frames: list[list[bool]] = []
    active = True
    output: list[str] = []
    for raw_line in text.splitlines():
        # The audited makefiles do not use escaped comment delimiters.  Removing
        # comments before semantic checks prevents a marker-only comment from
        # satisfying an object, rule, include, or link requirement.
        line = raw_line.split("#", 1)[0]
        directive = re.match(r"^[ \t]*!(IF|ELSEIF|ELSE|ENDIF)\b(.*)$", line, re.IGNORECASE)
        if directive is None:
            if active:
                output.append(line)
            continue

        keyword = directive.group(1).upper()
        expression = directive.group(2).strip()
        if keyword == "IF":
            parent_active = active
            value = _evaluate_nmake_condition(expression, configuration)
            known = value is not None
            taken = bool(value) if known else False
            frames.append([parent_active, known, taken])
            active = parent_active and (bool(value) if known else True)
        elif keyword == "ELSEIF" and frames:
            parent_active, known, taken = frames[-1]
            if not known:
                active = parent_active
                continue
            if taken:
                active = False
                continue
            value = _evaluate_nmake_condition(expression, configuration)
            if value is None:
                frames[-1][1] = False
                active = parent_active
            else:
                frames[-1][2] = bool(value)
                active = parent_active and bool(value)
        elif keyword == "ELSE" and frames:
            parent_active, known, taken = frames[-1]
            active = parent_active if not known else parent_active and not taken
            if known:
                frames[-1][2] = True
        elif keyword == "ENDIF" and frames:
            parent_active, _, _ = frames.pop()
            active = parent_active
    return "\n".join(output)


def _parse_nmake_variables(active_text: str) -> dict[str, str]:
    variables: dict[str, str] = {}
    lines = active_text.splitlines()
    index = 0
    while index < len(lines):
        match = re.match(r"^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=(.*)$", lines[index])
        if match is None:
            index += 1
            continue
        name = match.group(1).upper()
        value = match.group(2).rstrip()
        pieces: list[str] = []
        while value.endswith("\\"):
            pieces.append(value[:-1].strip())
            index += 1
            value = lines[index].rstrip() if index < len(lines) else ""
        pieces.append(value.strip())
        variables[name] = _normalize_makefile(" ".join(pieces))
        index += 1
    return variables


def _satisfied_makefile_checks(text: str, configuration: str) -> tuple[set[str], str]:
    active_text = active_nmake_text(text, configuration)
    normalized = _normalize_makefile(active_text)
    variables = _parse_nmake_variables(active_text)
    objects = " ".join((variables.get("OBJS", ""), variables.get("LINK32_OBJS", "")))
    compiler = variables.get("CPP_PROJ", "")
    linker = variables.get("LINK32_FLAGS", "")
    satisfied: set[str] = set()

    for name, object_name, source_name in MAKEFILE_SUBSTRATE_UNITS:
        if object_name in objects:
            satisfied.add(f"object:{name}")
        rule = re.compile(
            r'["\']?\$\(intdir\)/'
            + re.escape(object_name)
            + r'["\']?[ \t]*:[ \t]*'
            + re.escape(source_name)
            + r"(?:\s|$)"
        )
        if rule.search(normalized):
            satisfied.add(f"rule:{name}")
    for name, include in MAKEFILE_SUBSTRATE_INCLUDES:
        if include in compiler:
            satisfied.add(f"include:{name}")
    for name, library in MAKEFILE_SUBSTRATE_LIBRARIES:
        if re.search(r"(?<![A-Za-z0-9_.-])" + re.escape(library) + r"(?![A-Za-z0-9_.-])", linker):
            satisfied.add(f"library:{name}")
    return satisfied, normalized


def check_makefiles(root: Path) -> list[str]:
    errors: list[str] = []
    makefiles = {
        "v2": Path("v2.5-beta-1-modern/chat.mak"),
    }
    source: dict[str, str] = {}
    for name, relative in makefiles.items():
        path = root / relative
        if not path.is_file():
            errors.append(f"missing required Windows makefile: {relative.as_posix()}")
            source[name] = ""
        else:
            source[name] = _read_text(path)

    configurations = ("chat - Win32 Release", "chat - Win32 Debug")
    checks: dict[str, list[set[str]]] = {"v2": []}
    active_views: dict[str, list[str]] = {"v2": []}
    for name in makefiles:
        for configuration in configurations:
            satisfied, active_view = _satisfied_makefile_checks(source[name], configuration)
            checks[name].append(satisfied)
            active_views[name].append(active_view)

    for check in MAKEFILE_SUBSTRATE_CHECKS:
        missing_configs = [
            configuration
            for configuration, satisfied in zip(configurations, checks["v2"])
            if check not in satisfied
        ]
        if missing_configs:
            errors.append(
                f"v2 makefile does not prove {check} in: " + ", ".join(missing_configs)
            )

    for name, views in active_views.items():
        relative = makefiles[name]
        for marker in FORBIDDEN_MAKEFILE_TRANSPORT_MARKERS:
            if any(marker in view for view in views):
                errors.append(f"{relative.as_posix()} activates forbidden legacy TLS transport marker: {marker}")
    return errors


def audit_repository(root: Path) -> AuditResult:
    root = root.resolve()
    errors: list[str] = []
    required = (root / "AGENTS.md", root / "docs/TRANSPORT-RETIREMENT.md")
    for path in required:
        if not path.is_file():
            errors.append(f"not a Comic Chat repository root; missing {path.relative_to(root).as_posix()}")

    try:
        snapshots = load_product_sources(root)
    except (OSError, UnicodeError) as error:
        snapshots = {}
        errors.append(f"could not load modern product sources: {error}")

    try:
        v2_findings = scan_v2_legacy_ownership(root, snapshots)
        errors.extend(f"v2 legacy transport regression: {finding.render()}" for finding in v2_findings)
        allowed_network, forbidden_network = scan_network_ownership(root, snapshots)
        errors.extend(
            f"network API escaped shared engine allowlist: {finding.render()}"
            for finding in forbidden_network
        )
        schannel_findings = scan_schannel_activation(root, snapshots)
        errors.extend(f"SChannel transport activation: {finding.render()}" for finding in schannel_findings)
    except (OSError, UnicodeError) as error:
        allowed_network = []
        errors.append(f"could not scan modern product sources: {error}")

    try:
        makefile_errors = check_makefiles(root)
        errors.extend(makefile_errors)
    except (OSError, UnicodeError) as error:
        errors.append(f"could not verify Windows makefiles: {error}")

    return AuditResult(
        errors=errors,
        allowed_network_findings=allowed_network,
    )


def _print_result(result: AuditResult) -> None:
    if result.ok:
        print("transport ownership gate: PASS")
    else:
        print("transport ownership gate: FAIL", file=sys.stderr)
        for error in result.errors:
            print(f"  - {error}", file=sys.stderr)

    allowed_counts = Counter(finding.path for finding in result.allowed_network_findings)
    print("shared network implementation allowlist:")
    for path in sorted(NETWORK_IMPLEMENTATION_ALLOWLIST):
        print(f"  {path.as_posix()}: {allowed_counts.get(path, 0)} low-level calls")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (defaults to the parent of scripts/)",
    )
    args = parser.parse_args(argv)
    result = audit_repository(args.root)
    _print_result(result)
    return 0 if result.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
