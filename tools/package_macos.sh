#!/usr/bin/env bash
#
# Assemble a macOS release tarball from a finished build tree.
#
#   tools/package_macos.sh build/macos 1.1.0 [output-dir] [arch]
#
# The macOS counterpart of tools/package_linux.sh: the same named payload, the same unversioned
# top folder, and nothing ships until the result is proven to start on a Mac that has nothing
# installed. Dependencies outside the system are copied into lib/ and loaded through @rpath,
# which the executables resolve to @executable_path/lib. Rewriting a load command invalidates the
# code signature and Apple Silicon refuses to run unsigned code, so every binary is ad-hoc signed
# again afterwards.
#
# Libraries referenced through @rpath are looked up in GDIFFUSER_MACOS_LIB_PATH, a colon-separated
# list of folders (default /usr/local/lib), because the executables only carry the packaged rpath.
# GDIFFUSER_MACOS_MIN (default 11.0) is the oldest macOS every binary has to accept.
#
# Written for the bash 3.2 that ships with macOS: no mapfile, no associative arrays.

set -euo pipefail

if [ $# -lt 2 ] || [ $# -gt 4 ]; then
    echo "usage: $0 <build-dir> <version> [output-dir] [arch]" >&2
    exit 2
fi

if [ "$(uname -s)" != Darwin ]; then
    echo "error: $0 needs the macOS toolchain (otool, install_name_tool, codesign)" >&2
    exit 2
fi

build_dir=$1
version=$2
out_dir=${3:-dist}
arch=${4:-macos-arm64}
min_macos=${GDIFFUSER_MACOS_MIN:-11.0}
lib_path=${GDIFFUSER_MACOS_LIB_PATH:-/usr/local/lib}

case "$arch" in
    macos-arm64) cpu=arm64 ;;
    macos-x64) cpu=x86_64 ;;
    *)
        echo "error: unknown arch $arch (expected macos-arm64 or macos-x64)" >&2
        exit 2
        ;;
esac

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
    cp -R "$binary_dir/$item" "$stage/"
done
chmod +x "$stage/G-Diffuser" "$stage/gdx-extract" "$stage/gdx-update"

executables=("$stage/G-Diffuser" "$stage/gdx-extract" "$stage/gdx-update")
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

is_system_lib() {
    case "$1" in
        /usr/lib/* | /System/Library/*) return 0 ;;
    esac
    return 1
}

# Prints the libraries a Mach-O file loads, one per line, leaving out a dylib's own install name.
# Dependency lines are the tab-indented ones; the header names the file itself.
linked_libs() {
    local file=$1
    local own_id
    own_id=$(otool -D "$file" | sed -n 2p)
    otool -L "$file" | awk -v own="$own_id" '/^\t/ && $1 != own { print $1 }'
}

# Finds the file behind a load command, searching GDIFFUSER_MACOS_LIB_PATH for anything that is
# not an absolute path that exists.
resolve_lib() {
    local ref=$1
    if [ "${ref#/}" != "$ref" ] && [ -e "$ref" ]; then
        echo "$ref"
        return
    fi
    local name=${ref##*/}
    local IFS=:
    local dir
    for dir in $lib_path; do
        if [ -n "$dir" ] && [ -e "$dir/$name" ]; then
            echo "$dir/$name"
            return
        fi
    done
}

# True when an executable loads anything through @rpath but cannot find lib/. The output is
# captured before grep sees it: grep -q at the end of a pipe exits on the first match, and with
# pipefail the SIGPIPE it leaves behind would turn that match into a failure.
lacks_lib_rpath() {
    local refs rpaths
    refs=$(linked_libs "$1")
    rpaths=$(rpaths_of "$1")
    grep -q '^@rpath/' <<< "$refs" && ! grep -qx '@executable_path/lib' <<< "$rpaths"
}

# Prints the LC_RPATH entries of a Mach-O file, one per line. awk reads to the end instead of
# exiting early: with pipefail, closing the pipe makes otool die of SIGPIPE and aborts the script.
rpaths_of() {
    otool -l "$1" | awk '$1 == "cmd" && $2 == "LC_RPATH" { want = 1; next }
                         want && $1 == "path" { print $2; want = 0 }'
}

# Prints the minimum macOS a Mach-O file was built for, from LC_BUILD_VERSION or, on binaries
# built for older targets, LC_VERSION_MIN_MACOSX.
min_os_of() {
    otool -l "$1" | awk '$1 == "cmd" && ($2 == "LC_BUILD_VERSION" || $2 == "LC_VERSION_MIN_MACOSX") { want = 1; next }
                         want && ($1 == "minos" || $1 == "version") && !found { print $2; found = 1 }'
}

version_number() {
    echo "$1" | awk -F. '{ printf "%d\n", $1 * 10000 + $2 * 100 + $3 }'
}

# Copy every non-system dependency into lib/ and point its users at @rpath. Bundled dylibs are
# scanned too, so their own dependencies follow; the loop ends when a pass bundles nothing new.
mkdir -p "$stage/lib"
bundled_new=1
while [ "$bundled_new" -eq 1 ]; do
    bundled_new=0
    for file in "${executables[@]}" "$stage"/lib/*.dylib; do
        [ -e "$file" ] || continue
        linked_libs "$file" > "$scratch/libs"
        while read -r ref; do
            if is_system_lib "$ref"; then
                continue
            fi
            name=${ref##*/}
            if [ ! -e "$stage/lib/$name" ]; then
                source_lib=$(resolve_lib "$ref")
                if [ -z "$source_lib" ]; then
                    echo "error: cannot locate $ref, needed by ${file#"$stage"/}, to bundle it" >&2
                    exit 1
                fi
                cp -L "$source_lib" "$stage/lib/$name"
                chmod 755 "$stage/lib/$name"
                install_name_tool -id "@rpath/$name" "$stage/lib/$name"
                echo "bundled $name <- $source_lib"
                bundled_new=1
            fi
            if [ "$ref" != "@rpath/$name" ]; then
                install_name_tool -change "$ref" "@rpath/$name" "$file"
            fi
        done < "$scratch/libs"
    done
done
rmdir "$stage/lib" 2>/dev/null || true

# Only an executable that loads something through @rpath needs to know where lib/ is. The game
# gets the rpath from CMake; gdx-extract is a separate project and may not.
for exe in "${executables[@]}"; do
    if lacks_lib_rpath "$exe"; then
        install_name_tool -add_rpath @executable_path/lib "$exe"
    fi
done

machos=("${executables[@]}")
for dylib in "$stage"/lib/*.dylib; do
    if [ -e "$dylib" ]; then
        machos+=("$dylib")
    fi
done

for file in "${machos[@]}"; do
    codesign --force --sign - "$file"
done

# Check the finished tree, not the steps that built it. Every failure is listed before exiting so
# one run shows everything that is wrong.
failures=0
fail() {
    echo "FAIL: $1" >&2
    failures=$((failures + 1))
}

min_number=$(version_number "$min_macos")
for file in "${machos[@]}"; do
    label=${file#"$stage"/}

    archs=$(lipo -archs "$file")
    case " $archs " in
        *" $cpu "*) ;;
        *) fail "$label has no $cpu code (architectures: $archs)" ;;
    esac

    file_min=$(min_os_of "$file")
    if [ -z "$file_min" ]; then
        fail "$label does not record a minimum macOS"
    elif [ "$(version_number "$file_min")" -gt "$min_number" ]; then
        fail "$label requires macOS $file_min, newer than the supported $min_macos"
    fi

    linked_libs "$file" > "$scratch/libs"
    while read -r ref; do
        if is_system_lib "$ref"; then
            continue
        fi
        case "$ref" in
            @rpath/*)
                [ -e "$stage/lib/${ref#@rpath/}" ] || fail "$label loads $ref, which is not in lib/"
                ;;
            *)
                fail "$label loads $ref, which is neither a system library nor bundled in lib/"
                ;;
        esac
    done < "$scratch/libs"

    if ! codesign --verify --strict "$file" 2>/dev/null; then
        fail "$label has no valid signature"
    fi
done

for exe in "${executables[@]}"; do
    if lacks_lib_rpath "$exe"; then
        fail "${exe#"$stage"/} loads from @rpath but has no @executable_path/lib rpath"
    fi
done

if [ "$failures" -ne 0 ]; then
    echo "error: $failures portability check(s) failed; no tarball written" >&2
    exit 1
fi
echo "portable: ${#machos[@]} Mach-O file(s), macOS $min_macos or newer, $cpu"

# Fixed timestamps, ownership and sorted order so the same tree always produces the same tarball,
# matching tools/package_linux.sh. BSD tar has no --sort or --mtime, so the order comes from a
# sorted file list and the times are set on the files themselves. COPYFILE_DISABLE and
# --no-mac-metadata keep Finder metadata (._ files) out of the archive.
stamp=$(date -u -r "${SOURCE_DATE_EPOCH:-0}" +%Y%m%d%H%M.%S)
find "$stage" -exec env TZ=UTC0 touch -h -t "$stamp" {} +
(cd "$stage_root" && find "$(basename "$stage")" | LC_ALL=C sort) > "$scratch/files"

tarball="$out_dir/G-Diffuser-v$version-$arch.tar.gz"
COPYFILE_DISABLE=1 tar -c -f - --uid 0 --gid 0 --numeric-owner --no-mac-metadata --no-xattrs \
    -n -C "$stage_root" -T "$scratch/files" | gzip -n -9 > "$tarball"

echo
echo "$tarball"
shasum -a 256 "$tarball"
