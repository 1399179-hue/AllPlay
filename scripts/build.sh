#!/usr/bin/env bash
# 构建。用法： scripts/build.sh [额外的 cmake 参数...]
#   例：   scripts/build.sh -DCMAKE_BUILD_TYPE=Debug
# 环境变量：
#   BUILD_DIR   构建目录（默认 <项目根>/build）
#   TARGET      只编某个 target（如 allplay_host）
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT}/build}"

# macOS：brew 装在 /opt/homebrew，非交互 shell（ssh 进来跑脚本）不带它的 PATH /
# PKG_CONFIG_PATH；openssl@3 是 keg-only，必须显式给 OPENSSL_ROOT_DIR。
CMAKE_EXTRA=()
if [ "$(uname -s)" = "Darwin" ]; then
    BREW="$(command -v brew || echo /opt/homebrew/bin/brew)"
    if [ -x "$BREW" ]; then
        export PATH="$(dirname "$BREW"):$PATH"
        BREW_PREFIX="$("$BREW" --prefix)"
        export PKG_CONFIG_PATH="${BREW_PREFIX}/lib/pkgconfig:${BREW_PREFIX}/share/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
        CMAKE_EXTRA+=("-DOPENSSL_ROOT_DIR=$("$BREW" --prefix openssl@3)")
    fi
fi

JOBS="$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )"

# 注：macOS 自带 bash 3.2，空数组在 set -u 下直接展开会报 unbound variable，
# 所以用 ${arr[@]+...} 的写法。
cmake -S "$ROOT" -B "$BUILD_DIR" ${CMAKE_EXTRA[@]+"${CMAKE_EXTRA[@]}"} "$@"
if [ -n "${TARGET:-}" ]; then
    cmake --build "$BUILD_DIR" --target "$TARGET" -- -j"$JOBS"
else
    cmake --build "$BUILD_DIR" -- -j"$JOBS"
fi
echo "== 产物：$BUILD_DIR/ =="
