#!/usr/bin/env bash
# ============================================================================
# mac-bt-test.sh —— 在 Mac 的**桌面会话**里跑 core_selftest --bt（M2 真机测试）
# ============================================================================
# 为什么这么绕：macOS 27 的蓝牙隐私（TCC）只认 GUI 会话里的进程；ssh 进来的
# 进程枚举/查询蓝牙会被静默拒绝。做法：生成 .command → `open -a Terminal` 拉起
# （权限归 Terminal），输出落盘再回读。
#
# 用法（在容器里跑；SSID 可空，空则只测到 Wi-Fi 配置之前）：
#   scripts/mac-bt-test.sh <WiFi名字> [WiFi密码] [设备名过滤=iPhone]
# 之后：
#   ssh -p 2223 hewei@127.0.0.1 'tail -f /tmp/allplay-bt.log'   # 持续看
set -euo pipefail

SSID="${1-}"
PASS="${2:-}"
DEV="${3:-iPhone}"

: "${ALLPLAY_HOST:=hewei@127.0.0.1}"
: "${ALLPLAY_SSH_PORT:=2223}"
KEY="${ALLPLAY_SSH_KEY:-/root/.ssh/id_allplay}"
HOST="$ALLPLAY_HOST"
SSH_OPTS="-i ${KEY} -p ${ALLPLAY_SSH_PORT} -o StrictHostKeyChecking=no -o UserKnownHostsFile=/root/.ssh/known_hosts_remote"
LOG=/tmp/allplay-bt.log

# ---- 生成 .command（本地生成再 scp，避免多层引号地狱）----
tmp="$(mktemp)"
cat > "$tmp" <<EOF
#!/bin/bash
# 由 scripts/mac-bt-test.sh 生成（Mac 桌面会话执行）
cd "\${HOME}/AllPlay" || exit 1
echo "==== \$(date) 开始 ====" > ${LOG}
echo "== 环境 ==" >> ${LOG}
./build/core_selftest --bt --bt-device '${DEV}' --ssid '${SSID}' --wifi-pass '${PASS}' \
    --identity identity --screen 1280x720 >> ${LOG} 2>&1
echo "==== 进程退出（code=\$?）====" >> ${LOG}
EOF
chmod +x "$tmp"
cp "$tmp" /tmp/run-bt.command.local

scp -i "$KEY" -P "$ALLPLAY_SSH_PORT" \
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/root/.ssh/known_hosts_remote \
    "$tmp" "${HOST}:AllPlay/run-bt.command" >/dev/null
rm -f "$tmp"

echo "== 在 Mac 桌面会话拉起（TCC 归属 Terminal）=="
ssh $SSH_OPTS "$HOST" "chmod +x ~/AllPlay/run-bt.command && open -a Terminal ~/AllPlay/run-bt.command && echo 已拉起"

echo "== 等 8 秒后回读日志 =="
sleep 8
ssh $SSH_OPTS "$HOST" "cat ${LOG} 2>/dev/null || echo '(还没有输出)'"
echo
echo "== 之后可随时看： =="
echo "   ssh -i ${KEY} -p ${ALLPLAY_SSH_PORT} ${HOST} 'tail -40 ${LOG}'"
