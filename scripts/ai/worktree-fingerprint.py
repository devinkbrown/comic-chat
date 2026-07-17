#!/usr/bin/env python3
"""Fingerprint HEAD, tracked changes, and ignored-safe untracked file bytes."""

from __future__ import annotations

import argparse
import hashlib
import os
import stat
import subprocess
import sys
from pathlib import Path


def git(start: Path, *arguments: str) -> bytes:
    return subprocess.run(
        ["git", "-C", os.fspath(start), *arguments],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout


def add_record(digest: "hashlib._Hash", label: bytes, payload: bytes) -> None:
    digest.update(len(label).to_bytes(8, "big"))
    digest.update(label)
    digest.update(len(payload).to_bytes(8, "big"))
    digest.update(payload)


def reject_dirty_submodules(root: Path) -> None:
    """Reject initialized submodules whose internal bytes are not committed."""
    entries = git(root, "ls-files", "--stage", "-z").split(b"\0")
    for entry in entries:
        if not entry:
            continue
        metadata, separator, relative = entry.partition(b"\t")
        fields = metadata.split()
        if separator != b"\t" or len(fields) != 3 or fields[0] != b"160000" or fields[2] != b"0":
            continue
        path = root / os.fsdecode(relative)
        if not path.is_dir():
            continue
        probe = subprocess.run(
            ["git", "-C", os.fspath(path), "rev-parse", "--show-toplevel"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        if probe.returncode != 0 or Path(probe.stdout.strip()).resolve() != path.resolve():
            continue
        dirty = git(path, "status", "--porcelain=v1", "--untracked-files=all", "--ignore-submodules=none")
        if dirty:
            raise RuntimeError(
                f"dirty initialized submodule cannot be fingerprinted safely: {os.fsdecode(relative)}"
            )


def snapshot(root: Path) -> str:
    reject_dirty_submodules(root)
    digest = hashlib.sha256()
    add_record(digest, b"format", b"comicchat-worktree-v2")
    add_record(digest, b"head", git(root, "rev-parse", "HEAD").strip())
    add_record(
        digest,
        b"tracked-diff",
        git(root, "diff", "--binary", "--full-index", "--no-ext-diff", "HEAD", "--", "."),
    )

    untracked = git(root, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0")
    for relative in sorted(path for path in untracked if path):
        path = os.fsencode(root) + b"/" + relative
        before = os.lstat(path)
        mode = stat.S_IMODE(before.st_mode)
        add_record(digest, b"untracked-path", relative)
        add_record(digest, b"untracked-mode", f"{mode:o}".encode("ascii"))
        if stat.S_ISREG(before.st_mode):
            with open(path, "rb") as stream:
                content = stream.read()
            kind = b"regular"
        elif stat.S_ISLNK(before.st_mode):
            content = os.readlink(path)
            kind = b"symlink"
        else:
            raise RuntimeError(f"unsupported untracked file type: {os.fsdecode(relative)}")
        after = os.lstat(path)
        if (before.st_ino, before.st_size, before.st_mtime_ns, before.st_mode) != (
            after.st_ino,
            after.st_size,
            after.st_mtime_ns,
            after.st_mode,
        ):
            raise RuntimeError(f"file changed while hashing: {os.fsdecode(relative)}")
        add_record(digest, b"untracked-kind", kind)
        add_record(digest, b"untracked-content", content)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    arguments = parser.parse_args()
    root = Path(git(arguments.repo, "rev-parse", "--show-toplevel").decode().strip())
    first = snapshot(root)
    second = snapshot(root)
    if first != second:
        print("worktree changed while fingerprinting", file=sys.stderr)
        return 75
    print(first)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
