#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
legacy_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
manifest="$legacy_root/UPSTREAM-SHA256SUMS.txt"

if (( $# > 1 )); then
    printf 'Usage: %s [upstream-git-repository]\n' "$0" >&2
    exit 64
fi

if [[ ! -f "$manifest" ]]; then
    printf 'ERROR: missing checksum manifest: %s\n' "$manifest" >&2
    exit 2
fi

for tool in awk diff find grep sha256sum sort wc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'ERROR: required tool is unavailable: %s\n' "$tool" >&2
        exit 2
    fi
done

cd "$legacy_root"
sha256sum --check --strict --quiet UPSTREAM-SHA256SUMS.txt

actual_files=$(
    LC_ALL=C find LICENSE source artifacts artifacts-modern \
        \( -path 'source/Debug' -o -path 'source/Release' \) -prune -o \
        -type f ! -path 'source/icchat_i.c' -print |
        LC_ALL=C sort
)
manifest_files=$(
    awk '{ sub(/^[0-9a-fA-F]{64}  /, ""); print }' UPSTREAM-SHA256SUMS.txt |
        LC_ALL=C sort
)

if ! diff -u <(printf '%s\n' "$manifest_files") <(printf '%s\n' "$actual_files"); then
    printf 'ERROR: imported file inventory differs from the pinned snapshot.\n' >&2
    exit 1
fi

expected_revision='c7df00f60bc8e9fdef413f139e61f7c37e024684'
if ! grep -Fq "$expected_revision" PROVENANCE.md; then
    printf 'ERROR: PROVENANCE.md does not identify the pinned revision.\n' >&2
    exit 1
fi

printf 'PASS: %s imported files match upstream revision %s.\n' \
    "$(wc -l < UPSTREAM-SHA256SUMS.txt)" "$expected_revision"

if (( $# == 1 )); then
    upstream_repo=$1
    if ! command -v git >/dev/null 2>&1; then
        printf 'ERROR: git is required for upstream blob verification.\n' >&2
        exit 2
    fi
    if ! git -C "$upstream_repo" cat-file -e "$expected_revision^{commit}" 2>/dev/null; then
        printf 'ERROR: pinned revision is unavailable in %s.\n' "$upstream_repo" >&2
        exit 2
    fi

    blob_count=0
    while IFS= read -r local_path; do
        case "$local_path" in
            source/*)
                upstream_path="v2.5-beta-1-modern/${local_path#source/}"
                ;;
            *)
                upstream_path=$local_path
                ;;
        esac
        local_blob=$(git hash-object "$legacy_root/$local_path")
        upstream_blob=$(git -C "$upstream_repo" rev-parse "$expected_revision:$upstream_path")
        if [[ "$local_blob" != "$upstream_blob" ]]; then
            printf 'ERROR: Git blob mismatch: %s -> %s\n' \
                "$local_path" "$upstream_path" >&2
            exit 1
        fi
        blob_count=$((blob_count + 1))
    done < <(awk '{ sub(/^[0-9a-fA-F]{64}  /, ""); print }' UPSTREAM-SHA256SUMS.txt)

    printf 'PASS: %d imported files are byte-identical to pinned upstream Git blobs.\n' \
        "$blob_count"
fi
