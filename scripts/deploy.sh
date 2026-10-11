#!/usr/bin/env bash
# ============================================================================
# deploy.sh —— 把项目同步到目标机并（可选）远程构建 / 跑 M1 验收
# ============================================================================
# 用法：
#   scripts/deploy.sh                  # 默认同步到 Ubuntu VM（走 2222 隧道）
#   scripts/deploy.sh --build          # 同步后远程构建
#   scripts/deploy.sh --build --run-m1 # 同步 + 构建 + 远程 M1 验收
#   scripts/deploy.sh --mac ...        # 目标换成 Mac（走 2223 隧道）
#
# 环境变量（都可覆盖）：
#   ALLPLAY_TARGET=ubuntu|mac  ALLPLAY_HOST=...  ALLPLAY_SSH_PORT=...  ALLPLAY_SSH_KEY=...
#   ALLPLAY_DEST=...
#
# ⚠️ identity/ 会被同步过去（目标机上运行必需），但它**是本项目的敏感文件**：
#    别把它拷去任何别的地方、别进任何仓库。
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"

TARGET="${ALLPLAY_TARGET:-ubuntu}"
DO_BUILD=0
DO_M1=0
for a in "$@"; do
    case "$a" in
        --mac)      TARGET=mac ;;
        --ubuntu)   TARGET=ubuntu ;;
        --build)    DO_BUILD=1 ;;
        --run-m1)   DO_BUILD=1; DO_M1=1 ;;
        *) echo "未知参数：$a" >&2; exit 2 ;;
    esac
done

if [ "$TARGET" = "mac" ]; then
    : "${ALLPLAY_HOST:=hewei@127.0.0.1}"
    : "${ALLPLAY_SSH_PORT:=2223}"
    : "${ALLPLAY_DEST:=/Users/hewei/AllPlay}"
else
    : "${ALLPLAY_HOST:=blt@127.0.0.1}"
    : "${ALLPLAY_SSH_PORT:=2222}"
    : "${ALLPLAY_DEST:=/home/blt/AllPlay}"
fi
KEY="${ALLPLAY_SSH_KEY:-/root/.ssh/id_allplay}"
HOST="$ALLPLAY_HOST"; PORT="$ALLPLAY_SSH_PORT"; DEST="$ALLPLAY_DEST"
SSH_OPTS="-i ${KEY} -p ${PORT} -o StrictHostKeyChecking=no -o UserKnownHostsFile=/root/.ssh/known_hosts_remote"

echo "== 同步 $ROOT → ${HOST}:${DEST}（目标=$TARGET） =="
ssh $SSH_OPTS "$HOST" "mkdir -p '${DEST}'"
rsync -az --delete -e "ssh ${SSH_OPTS}" \
      --exclude 'build/' --exclude '*.o' \
      "${ROOT}/" "${HOST}:${DEST}/"
echo "== 同步完成 =="

if [ "$DO_BUILD" = 1 ]; then
    echo "== 远程构建 =="
    ssh $SSH_OPTS "$HOST" "bash '${DEST}/scripts/build.sh'" 2>&1 | tail -25
fi

if [ "$DO_M1" = 1 ]; then
    echo "== 远程跑 M1 验收 =="
    ssh $SSH_OPTS "$HOST" "bash '${DEST}/scripts/m1-check.sh'"
fi
