#!/usr/bin/env bash
# Prove GitHub #23-B on Linux: Clang must compile vendored discord-rpc
# after the RapidJSON GenericStringRef::operator= deletion. Isolated from
# the Ally GCC Release tree (build/x64-linux-release).
set -euo pipefail

echo "=== host ==="
uname -a
echo "=== gcc ==="
gcc --version | head -n 1
echo "=== clang probe ==="
if ! command -v clang++ >/dev/null 2>&1; then
    echo "CLANG_MISSING: clang++ not on PATH"
    exit 2
fi
clang++ --version | head -n 2

echo "=== issue #23 TU compile ==="
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
inc=(-Iport/third_party/discord-rpc/include -Iport/third_party/discord-rpc/thirdparty)
failed=0
for src in serialization rpc_connection discord_rpc; do
    echo "---- ${src}.cpp ----"
    if ! clang++ -std=gnu++20 -O3 -DNDEBUG -w "${inc[@]}" \
        -c "port/third_party/discord-rpc/src/${src}.cpp" \
        -o "${tmpdir}/${src}.o"; then
        failed=1
    fi
done
if [ "$failed" -ne 0 ]; then
    echo "TU_COMPILE_FAILED"
    exit 1
fi
echo "TU_COMPILE_OK"

vcpkg_root="$PWD/build/x64-linux-release/libultraship/vcpkg"
if [ ! -d "$vcpkg_root" ]; then
    echo "CMAKE_SKIPPED: no existing vcpkg at $vcpkg_root"
    exit 0
fi

echo "=== cmake discord-rpc (clang, build/x64-linux-clang) ==="
sed -i 's/\r$//' libultraship/cmake/dependencies/patches/*.patch || true
export VCPKG_ROOT="$vcpkg_root"
cmake -S . -B build/x64-linux-clang -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DGDX_FORCE_DEV_TOOLS=ON
cmake --build build/x64-linux-clang --target discord-rpc -j
echo "CMAKE_DISCORD_RPC_OK"
