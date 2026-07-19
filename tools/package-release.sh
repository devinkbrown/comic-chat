#!/usr/bin/env bash
# Build reproducible x86_64 Windows and Linux ComicChat release archives.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
version=${1:-$(git -C "$repo_root" describe --tags --always --dirty)}
output_dir=${OUTPUT_DIR:-"$repo_root/dist"}
stage_dir=$(mktemp -d)
trap 'rm -rf "$stage_dir"' EXIT

package_target() {
    local target=$1 platform=$2 executable=$3 archive=$4 format=$5
    local prefix="$stage_dir/$platform"
    local package_dir="$prefix/comicchat-$version-$platform"

    (cd "$repo_root" && zig build -Dtarget="$target" -Doptimize=ReleaseSafe -p "$prefix")
    mkdir -p "$package_dir"
    cp "$prefix/bin/$executable" "$package_dir/"
    cp "$repo_root/README.md" "$repo_root/LICENSE" "$package_dir/"

    if [[ "$format" == zip ]]; then
        (cd "$prefix" && 7z a -tzip -bd "$output_dir/$archive" "$(basename "$package_dir")")
    else
        tar -C "$prefix" -czf "$output_dir/$archive" "$(basename "$package_dir")"
    fi
}

mkdir -p "$output_dir"
rm -f "$output_dir"/comicchat-"$version"-windows-x86_64.zip \
      "$output_dir"/comicchat-"$version"-linux-x86_64.tar.gz \
      "$output_dir"/comicchat-"$version"-freebsd-x86_64.tar.gz \
      "$output_dir"/comicchat-"$version"-openbsd-x86_64.tar.gz \
      "$output_dir"/comicchat-"$version"-SHA256SUMS.txt

package_target x86_64-windows windows-x86_64 comicchat.exe \
    "comicchat-$version-windows-x86_64.zip" zip
package_target x86_64-linux linux-x86_64 comicchat \
    "comicchat-$version-linux-x86_64.tar.gz" tar.gz
package_target x86_64-freebsd freebsd-x86_64 comicchat \
    "comicchat-$version-freebsd-x86_64.tar.gz" tar.gz
package_target x86_64-openbsd openbsd-x86_64 comicchat \
    "comicchat-$version-openbsd-x86_64.tar.gz" tar.gz

(cd "$output_dir" && sha256sum \
    "comicchat-$version-windows-x86_64.zip" \
    "comicchat-$version-linux-x86_64.tar.gz" \
    "comicchat-$version-freebsd-x86_64.tar.gz" \
    "comicchat-$version-openbsd-x86_64.tar.gz" \
    > "comicchat-$version-SHA256SUMS.txt")

printf 'Packages written to %s\n' "$output_dir"
