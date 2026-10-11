#!/usr/bin/env bash
# ============================================================================
# m1-check.sh —— M1 验收：RTSP + /info(bplist) + Bonjour 广播
# ============================================================================
# 在任何一台装好依赖的机器上跑（容器 / 目标机皆可）。
#
# ⚠️ 为什么不用 curl 验 /info：AirPlay 的应答状态行是 `RTSP/1.0 200 OK`，
#    curl 只认 HTTP/1.x，会静默报 "Received HTTP/0.9 when not allowed" 并把
#    响应丢掉 —— 服务端没问题，是工具不认。这里用原始 TCP 客户端（也正好贴近
#    手机实际行为）。
#
# 步骤：
#   1) 起 core_selftest（--no-avahi）→ 2) 原始 RTSP：GET /info + OPTIONS
#   3) plist 逐字段断言 → 4) 若 avahi 活跃且有 avahi-browse：再验广播可见性
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
BIN="${ALLPLAY_SELFTEST:-${ROOT}/build/core_selftest}"
PORT="${M1_PORT:-17000}"
STATE_DIR="${ALLPLAY_STATE_DIR:-${ROOT}/build/m1-state}"
LOG=/tmp/m1-check.log
FAIL=0

say()  { printf '%s\n' "$*"; }
fail() { printf '❌ %s\n' "$*"; FAIL=1; }
ok()   { printf '✅ %s\n' "$*"; }

[ -x "$BIN" ] || { say "找不到 ${BIN}（先 scripts/build.sh）"; exit 2; }

cleanup() { if [ -n "${PID:-}" ]; then kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; fi; }
trap cleanup EXIT

say "== ① 起 core_selftest（--no-avahi，port=${PORT}）=="
"$BIN" --port "$PORT" --no-avahi --state-dir "$STATE_DIR" >"$LOG" 2>&1 &
PID=$!
for i in $(seq 1 50); do
    grep -q "READY" "$LOG" && break
    sleep 0.2
done
grep -q "READY" "$LOG" || { say "---- 日志 ----"; cat "$LOG"; fail "core 未能就绪"; exit 1; }
grep -E "^\[selftest\]" "$LOG" | sed 's/^/   /'
DEVID=$(grep -oE "deviceid=[0-9A-F:]+" "$LOG" | head -1 | cut -d= -f2)

say "== ②③ 原始 RTSP：GET /info + OPTIONS，并校验 bplist =="
if python3 - "$PORT" "$DEVID" <<'PY'
import plistlib, socket, sys

port, devid = int(sys.argv[1]), sys.argv[2]

def rtsp(method, uri):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    req = f"{method} {uri} RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: m1-check\r\n\r\n"
    s.sendall(req.encode())
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = s.recv(65536)
        if not chunk: break
        data += chunk
    head, _, body = data.partition(b"\r\n\r\n")
    lines = head.decode(errors="replace").split("\r\n")
    hdr = {}
    for ln in lines[1:]:
        if ":" in ln:
            k, v = ln.split(":", 1)
            hdr[k.strip().lower()] = v.strip()
    clen = int(hdr.get("content-length", "0"))
    while len(body) < clen:
        chunk = s.recv(65536)
        if not chunk: break
        body += chunk
    s.close()
    return lines[0], hdr, body

failures = []
def chk(cond, msg):
    print(("   ✅ " if cond else "   ❌ ") + msg)
    if not cond: failures.append(msg)

# ---- GET /info ----
status, hdr, body = rtsp("GET", "/info")
chk(status.startswith("RTSP/1.0 200"), f"/info 状态行：{status}")
chk(hdr.get("content-type") == "application/x-apple-binary-plist",
    f"/info Content-Type：{hdr.get('content-type')}")
try:
    d = plistlib.loads(body)
except Exception as e:
    chk(False, f"plist 解析失败：{e}")
    d = {}
chk(d.get("deviceID") == devid, f"deviceID == {devid}（实际 {d.get('deviceID')}）")
chk(d.get("features") == 0x615653AEE2, "features == 0x615653AEE2")
chk(len(d.get("pi", "")) == 36, "pi 是 UUID")
chk(len(d.get("pk", "")) == 64, "pk 是 32 字节 hex")
disps = d.get("displays") or []
chk(len(disps) == 1 and disps[0].get("widthPixels") == 1280
    and disps[0].get("heightPixels") == 720, "displays[0] = 1280x720")
chk(d.get("srcvers") == "370.10" and d.get("protovers") == "1.1", "srcvers/protovers")

# ---- OPTIONS ----
status, hdr, _ = rtsp("OPTIONS", "/")
chk(status.startswith("RTSP/1.0 200"), f"OPTIONS 状态行：{status}")
chk("public" in hdr and "SETUP" in hdr.get("public", ""), "OPTIONS 带 Public（含 SETUP）")

sys.exit(1 if failures else 0)
PY
then ok "/info + OPTIONS 全过"; else fail "/info 或 OPTIONS 校验失败"; fi

say "== ④ Bonjour（按平台选验证工具）=="
if [ "$(uname -s)" = "Darwin" ]; then
    # macOS：dns_sd 后端；用系统 dns-sd 浏览。没有 timeout 命令，用后台+kill。
    if command -v dns-sd >/dev/null 2>&1; then
        kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
        "$BIN" --port "$PORT" --state-dir "$STATE_DIR" >"$LOG" 2>&1 &
        PID=$!
        for i in $(seq 1 50); do grep -q "READY" "$LOG" && break; sleep 0.2; done
        grep -E "Bonjour" "$LOG" | sed 's/^/   /'
        dns-sd -B _airplay._tcp > /tmp/m1-dns-sd.log 2>&1 &
        DSPID=$!
        sleep 3
        kill "$DSPID" 2>/dev/null
        grep -qi 'CarPlay' /tmp/m1-dns-sd.log && ok "dns-sd 能看到本机 _airplay._tcp" \
                                              || fail "dns-sd 没找到本机服务"
    else
        say "   （跳过：没有 dns-sd）"
    fi
elif command -v avahi-browse >/dev/null 2>&1 && systemctl is-active avahi-daemon >/dev/null 2>&1; then
    kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
    "$BIN" --port "$PORT" --state-dir "$STATE_DIR" >"$LOG" 2>&1 &
    PID=$!
    for i in $(seq 1 50); do grep -q "READY" "$LOG" && break; sleep 0.2; done
    grep -E "Bonjour" "$LOG" | sed 's/^/   /'

    # 判据用"服务实例可见"（+ 行），不押在 TXT 解析上：
    # 办公室局域网里 AirPlay 设备多，-r 解析会排队，短超时下 TXT 可能还没回来。
    # 实例名里的空格被 avahi-browse -p 转义成 \032。
    FOUND=0; TXTOK=0
    for i in 1 2 3; do
        out="$(timeout 8 avahi-browse -r -t -p _airplay._tcp 2>/dev/null)"
        printf '%s' "$out" | grep -qF 'CarPlay\032Linux' && FOUND=1
        printf '%s' "$out" | grep -qF "${DEVID}" && TXTOK=1
        [ "$FOUND" = 1 ] && break
    done
    [ "$FOUND" = 1 ] && ok "avahi-browse 能看到本机 _airplay._tcp 服务" \
                     || fail "avahi-browse 没找到本机服务"
    [ "$TXTOK" = 1 ] && ok "TXT 解析到 deviceid=${DEVID}" \
                     || say "   （TXT 解析这次没排上队，服务可见即可）"
else
    say "   （跳过：没有 avahi-browse 或 avahi-daemon 未运行）"
fi

cleanup; PID=""
echo
if [ "$FAIL" = 0 ]; then say "==== M1 验收：全部通过 ===="; else say "==== M1 验收：有失败 ===="; tail -30 "$LOG"; fi
exit "$FAIL"
