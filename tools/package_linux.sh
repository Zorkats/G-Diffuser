#!/usr/bin/env bash
#
# Assemble a Linux release tarball from a finished build tree.
#
#   tools/package_linux.sh [--companion] build/x64-linux-release 1.1.0 [output-dir] [arch]
#
# The payload is listed by name rather than copied with a wildcard: a release that quietly
# lost decomp-recipes/ would still boot on the machine that built it and fail everywhere
# else, so a missing piece has to be a hard error here. Dependencies that no distribution
# can be relied on to provide are copied into lib/, which the executable finds through its
# $ORIGIN RUNPATH. Nothing ships until check_linux_abi.py agrees the result is portable.
#
# The arch defaults to linux-x64; pass linux-arm64 (or set GDIFFUSER_LINUX_ARCH) to build
# an ARM64 tarball. Existing callers that omit it keep the original behavior.
#
# With --companion, the G-Diffuser Companion is staged into G-Diffuser/companion/: its executable,
# music_targets.json, and the frozen worker folder (tools/build_companion_worker.py must have run,
# then a build of G-Diffuser-Companion-worker). Its SDL2 comes from the same lib/ as the game's,
# the frozen worker passes the same ABI limits, and the staged copy must pass its own --selftest.
# User themes in the build tree are test data and are not shipped.

set -euo pipefail

companion=0
if [ "${1:-}" = "--companion" ]; then
    companion=1
    shift
fi

if [ $# -lt 2 ] || [ $# -gt 4 ]; then
    echo "usage: $0 [--companion] <build-dir> <version> [output-dir] [arch]" >&2
    exit 2
fi

build_dir=$1
version=$2
out_dir=${3:-dist}
arch=${4:-${GDIFFUSER_LINUX_ARCH:-linux-x64}}
repo_root=$(cd "$(dirname "$0")/.." && pwd)

binary_dir="$build_dir/port"
if [ ! -x "$binary_dir/G-Diffuser" ]; then
    echo "error: no G-Diffuser executable in $binary_dir" >&2
    echo "       build first: cmake --build $build_dir --target G-Diffuser" >&2
    exit 1
fi

# The tarball's top folder is unversioned. Saves live next to the executable, so extracting an
# update over the previous install keeps them; a versioned folder would start every update
# without saves (GitHub #41). The staging root stays versioned so packages never collide.
stage_root="$out_dir/G-Diffuser-v$version-$arch"
rm -rf "$stage_root"
stage="$stage_root/G-Diffuser"
mkdir -p "$stage"

# Everything a fresh install needs. gdx-extract and decomp-recipes/ are what turn the user's
# ROM into generic.o2r on first boot; without them the game starts and then has no assets.
# gdx-update is the helper that replaces the build later, so it ships with what it updates.
payload=(
    G-Diffuser
    gdx-extract
    gdx-update
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
chmod +x "$stage/G-Diffuser" "$stage/gdx-extract" "$stage/gdx-update"
executables=("$stage/G-Diffuser" "$stage/gdx-extract" "$stage/gdx-update")

if [ "$companion" = 1 ]; then
    # The Companion finds the game in the folder above its own, so it lives in a companion subfolder.
    companion_bin="$build_dir/companion"
    companion_stage="$stage/companion"
    mkdir -p "$companion_stage"
    for item in G-Diffuser-Companion music_targets.json worker; do
        if [ ! -e "$companion_bin/$item" ]; then
            echo "error: companion build is missing $item" >&2
            echo "       freeze the worker: python3 -I tools/build_companion_worker.py, then" >&2
            echo "       cmake --build $build_dir --target G-Diffuser-Companion G-Diffuser-Companion-worker" >&2
            exit 1
        fi
        cp -r "$companion_bin/$item" "$companion_stage/"
    done
    chmod +x "$companion_stage/G-Diffuser-Companion" "$companion_stage/worker/gdx_companion_worker"
    for license in MIT-CMU-Pillow.txt PSF-2.0-Python.txt; do
        if [ ! -f "$companion_stage/worker/_internal/LICENSES/$license" ]; then
            echo "error: packaging refused: the frozen worker has no $license; freeze it again" >&2
            exit 1
        fi
    done
    executables+=("$companion_stage/G-Diffuser-Companion")
fi

mkdir -p "$stage/lib"
mapfile -t bundle < <(python3 "$repo_root/tools/check_linux_abi.py" --list-unbundled "${executables[@]}")
for soname in "${bundle[@]}"; do
    # ldd resolves through the same search path the loader would use, so this picks up the
    # versions actually linked against rather than whatever else is installed. Requiring an
    # absolute path skips the "=> not found" lines, which would otherwise yield the word "not".
    # awk must NOT exit on the first match: with pipefail, closing the pipe early makes ldd die
    # of SIGPIPE and the whole script aborts with 141 on hosts where the match lands early.
    resolved=$(ldd "${executables[@]}" \
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
frozen_args=()
[ "$companion" = 1 ] && frozen_args=(--frozen-dir "$companion_stage/worker")
python3 "$repo_root/tools/check_linux_abi.py" "${lib_args[@]}" "${frozen_args[@]}" "${executables[@]}"

if [ "$companion" = 1 ]; then
    # The staged copy runs its worker handshake with the bundled lib/, which also proves the
    # RUNPATH and the worker folder are complete. --selftest opens no window.
    selftest=$("$companion_stage/G-Diffuser-Companion" --selftest 2>&1) || {
        echo "error: packaging refused: the staged Companion failed its selftest" >&2
        echo "$selftest" >&2
        exit 1
    }
    case "$selftest" in
        *"selftest: PASS"*) echo "$selftest" ;;
        *)
            echo "error: packaging refused: the staged Companion did not report selftest: PASS" >&2
            echo "$selftest" >&2
            exit 1
            ;;
    esac
fi

# Fixed ownership and sorted order so the same tree always produces the same tarball,
# matching the determinism the asset extractor already guarantees.
tarball="$out_dir/G-Diffuser-v$version-$arch.tar.gz"
tar --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@${SOURCE_DATE_EPOCH:-0}" \
    -czf "$tarball" -C "$stage_root" "$(basename "$stage")"

echo
echo "$tarball"
sha256sum "$tarball"
