#!/bin/sh
set -eu

EXPECTED_REVISION=c7df00f60bc8e9fdef413f139e61f7c37e024684
EXPECTED_SHA256=67f176d23c1701918498fffe148dfb67f2fc836850ccf29f26647ed547fe292d

if [ "$#" -ne 1 ]; then
    echo "usage: $0 /path/to/microsoft-comic-chat" >&2
    exit 2
fi

upstream=$1
source_file=$upstream/v2.5-beta-1-modern/comicart/xeno.avb
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(dirname -- "$script_dir")
destination=$repo_root/src/assets/testdata/xeno.avb

actual_revision=$(git -C "$upstream" rev-parse HEAD)
if [ "$actual_revision" != "$EXPECTED_REVISION" ]; then
    echo "refusing Xeno import: expected upstream revision $EXPECTED_REVISION, got $actual_revision" >&2
    exit 1
fi

if [ ! -f "$source_file" ]; then
    echo "refusing Xeno import: missing $source_file" >&2
    exit 1
fi

actual_source_sha=$(sha256sum "$source_file" | awk '{print $1}')
if [ "$actual_source_sha" != "$EXPECTED_SHA256" ]; then
    echo "refusing Xeno import: expected SHA-256 $EXPECTED_SHA256, got $actual_source_sha" >&2
    exit 1
fi

temporary=$destination.tmp.$$
trap 'rm -f "$temporary"' EXIT HUP INT TERM
cp "$source_file" "$temporary"
chmod 0644 "$temporary"
mv "$temporary" "$destination"
trap - EXIT HUP INT TERM

actual_destination_sha=$(sha256sum "$destination" | awk '{print $1}')
if [ "$actual_destination_sha" != "$EXPECTED_SHA256" ] || ! cmp -s "$source_file" "$destination"; then
    echo "Xeno import verification failed" >&2
    exit 1
fi

echo "imported byte-identical pinned Xeno asset: $actual_destination_sha"
