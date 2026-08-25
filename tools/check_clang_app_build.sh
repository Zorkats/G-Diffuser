#!/usr/bin/env bash
# Full Clang application build of G-Diffuser on Linux, isolated from the
# Ally GCC Release tree at build/x64-linux-release.
set -euo pipefail

build_dir=build/x64-linux-clang
log_prefix="[clang-app]"

echo "${log_prefix} host=$(uname -srm)"
echo "${log_prefix} pwd=$PWD"
clang++ --version | head -n 2
echo "${log_prefix} sed patches"
sed -i 's/\r$//' libultraship/cmake/dependencies/patches/*.patch || true

echo "${log_prefix} cmake configure ${build_dir}"
stdbuf -oL -eL cmake -S . -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DGDX_FORCE_DEV_TOOLS=ON

echo "${log_prefix} cmake build G-Diffuser"
stdbuf -oL -eL cmake --build "${build_dir}" --target G-Diffuser -j

bin="${build_dir}/port/G-Diffuser"
if [ ! -x "$bin" ]; then
    echo "${log_prefix} APP_BUILD_FAILED: missing $bin"
    exit 1
fi

echo "${log_prefix} binary"
ls -l "$bin"
file "$bin" || true
echo "${log_prefix} cxx=$(grep -E '^CMAKE_CXX_COMPILER:' "${build_dir}/CMakeCache.txt" || true)"
if ldd "$bin" | grep -i 'not found'; then
    echo "${log_prefix} APP_BUILD_LINK_OK_BUT_MISSING_LIBS"
    exit 1
fi
echo "${log_prefix} LDLIBS_OK"
echo "${log_prefix} APP_BUILD_OK"
