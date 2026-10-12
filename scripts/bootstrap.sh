#!/usr/bin/env bash
# ============================================================================
# bootstrap.sh —— 装齐构建/运行依赖（目标 Ubuntu 机 & 本开发容器通用）
# ============================================================================
# 覆盖的依赖：
#   - 构建：build-essential / cmake / pkg-config
#   - 核心：libssl-dev（AES/SHA/ECDSA）；libbluetooth-dev + bluez（无线触发的 RFCOMM/SDP）
#   - 宿主：libsdl2-dev + libsdl2-ttf-dev（窗口/状态栏）；libavcodec/libavutil/libswscale-dev（H.264 解码 + 像素格式转换）
#   - 预留：libusb-1.0-0-dev（未来有线 CarPlay/USBMUX）、libdbus-1-dev（BlueZ D-Bus 备选路径）
# 注意：
#   · **不带** fdk-aac —— Apple 端音频走 AAC-LC/Opus/LPCM，一律用 ffmpeg 自带解码器。
#   · **不带** libswresample-dev —— 宿主只用"平面 float → 交织 s16"这一种转换，自己写，
#     顺带绕开它在 ffmpeg 4.4 / 5.1+ 之间那套 channel-layout 参数签名差异。
#   · libavformat-dev 本项目用不到（无容器/无解复用），但留着无害。
set -euo pipefail

SUDO=""
if [ "$(id -u)" != 0 ]; then SUDO="sudo"; fi

echo "== apt update =="
$SUDO apt-get update -y

echo "== 安装依赖 =="
$SUDO apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libssl-dev libusb-1.0-0-dev libdbus-1-dev libbluetooth-dev bluez \
    libavahi-client-dev \
    libsdl2-dev libsdl2-ttf-dev \
    libavcodec-dev libavformat-dev libswscale-dev libavutil-dev \
    fonts-dejavu-core

# mDNS 用系统 avahi（广播 _airplay._tcp、发现 _carplay-ctrl._tcp）：
# 确认 avahi-daemon 是启用的；没起的话 M1 的 Bonjour 断言会失败。
if command -v systemctl >/dev/null 2>&1 && ! systemctl is-active avahi-daemon >/dev/null 2>&1; then
    echo
    echo "⚠️  avahi-daemon 当前未运行。M1 的 Bonjour 广播需要它（核心经 D-Bus 调 avahi）："
    echo "      sudo systemctl enable --now avahi-daemon"
fi

echo
echo "== 完成。下一步 =="
echo "  1) 准备 MFi 身份：identity/identity.pk8 + identity/certificate.p7b（见 docs/README.md）"
echo "  2) scripts/build.sh              # 构建"
echo "  3) scripts/run.sh --help"
