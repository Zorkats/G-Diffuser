#!/usr/bin/env bash
#
# Assemble a Linux release tarball from a finished build tree.
#
#   tools/package_linux.sh build/x64-linux-release 1.1.0 [output-dir]
#
# The payload is listed by name rather than copied with a wildcard: a release that quietly
# lost decomp-recipes/ would still boot on the machine that built it and fail everywhere
# else, so a missing piece has to be a hard error here. Dependencies that no distribution
# can be relied on to provide are copied into lib/, which the executable finds through its
# $ORIGIN RUNPATH. Nothing ships until check_linux_abi.py agrees the result is portable.

set -euo pipefail

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
    echo "usage: $0 <build-dir> <version> [output-dir]" >&2
    exit 2
fi

build_dir=$1
version=$2
out_dir=${3:-dist}
repo_root=$(cd "$(dirname "$0")/.." && pwd)

binary_dir="$build_dir/port"
if [ ! -x "$binary_dir/G-Diffuser" ]; then
    echo "error: no G-Diffuser executable in $binary_dir" >&2
    echo "       build first: cmake --build $build_dir --target G-Diffuser" >&2
    exit 1
fi

stage="$out_dir/G-Diffuser-v$version-linux-x64"
rm -rf "$stage"
mkdir -p "$stage"

# Everything a fresh install needs. gdx-extract and decomp-recipes/ are what turn the user's
# ROM into generic.o2r on first boot; without them the game starts and then has no assets.
payload=(
    G-Diffuser
    gdx-extract
    gdiffuser.o2r
    gamecontrollerdb.txt
    fonts
    decomp-recipes
    shaders
    LICENSE
    THIRD_PARTY_NOTICES.md
    LICENSES
)
for item in "${payload[@]}"; do
    if [ ! -e "$binary_dir/$item" ]; then
        echo "error: build tree is missing $item" >&2
        exit 1
    fi
    cp -r "$binary_dir/$item" "$stage/"
done
chmod +x "$stage/G-Diffuser" "$stage/gdx-extract"

mkdir -p "$stage/lib"
mapfile -t bundle < <(python3 "$repo_root/tools/check_linux_abi.py" --list-unbundled \
    "$stage/G-Diffuser" "$stage/gdx-extract")
for soname in "${bundle[@]}"; do
    # ldd resolves through the same search path the loader would use, so this picks up the
    # versions actually linked against rather than whatever else is installed. Requiring an
    # absolute path skips the "=> not found" lines, which would otherwise yield the word "not".
    # awk must NOT exit on the first match: with pipefail, closing the pipe early makes ldd die
    # of SIGPIPE and the whole script aborts with 141 on hosts where the match lands early.
    resolved=$(ldd "$stage/G-Diffuser" "$stage/gdx-extract" \
        | awk -v want="$soname" '$1 == want && $3 ~ /^\// && !found { print $3; found=1 }')
    if [ -z "$resolved" ] || [ ! -e "$resolved" ]; then
        echo "error: cannot locate $soname to bundle it" >&2
        exit 1
    fi
    cp -L "$resolved" "$stage/lib/$soname"
    echo "bundled $soname <- $resolved"
done
rmdir "$stage/lib" 2>/dev/null || true

lib_args=()
[ -d "$stage/lib" ] && lib_args=(--lib-dir "$stage/lib")
python3 "$repo_root/tools/check_linux_abi.py" "${lib_args[@]}" \
    "$stage/G-Diffuser" "$stage/gdx-extract"

# Fixed ownership and sorted order so the same tree always produces the same tarball,
# matching the determinism the asset extractor already guarantees.
tarball="$out_dir/G-Diffuser-v$version-linux-x64.tar.gz"
tar --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@${SOURCE_DATE_EPOCH:-0}" \
    -czf "$tarball" -C "$out_dir" "$(basename "$stage")"

echo
echo "$tarball"
sha256sum "$tarball"
