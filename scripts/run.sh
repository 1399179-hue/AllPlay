#!/usr/bin/env bash
# 运行宿主程序（默认挂 build/allplay_host）。
# 用法： scripts/run.sh --screen 1280x720 [--fullscreen] ...
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
BIN="${ALLPLAY_HOST_BIN:-${ROOT}/build/allplay_host}"

if [ ! -x "$BIN" ]; then
    echo "找不到 $BIN —— 先跑 scripts/build.sh" >&2
    exit 1
fi
exec "$BIN" "$@"
