/**
 * @file   device_identity.h
 * @brief  本机设备身份（Ed25519 长期密钥 + 配对 UUID）
 * @author blt
 */

// ============================================================================
// core/platform/device_identity.h —— 本机设备身份（Ed25519 长期密钥 + 配对 UUID）
// ============================================================================
// 为什么必须持久化：手机把"这台接收端"记成配对过的设备，认的是这个 Ed25519
// 公钥（0x4301 里推给手机的那个 pk）。进程重启 / 机器重启后必须**还是同一把**，
// 否则手机的配对记录失效，每次都要重新走配对流程。
//
// 与 identity/（MFi 配件身份 pk8/p7b）是两码事：
//   · 这里是 **AirPlay 配对层**的身份（Ed25519）——每台机器各自生成；
//   · identity/ 是 **Apple MFi 认证**的身份——所有机器共用同一份（单独引入的凭证文件，非代码）。
#pragma once

#include <openssl/evp.h>

#include <cstdint>
#include <string>
#include <vector>

namespace core {

struct DeviceIdentity {
    std::string          pi;      ///< pairing id（UUID 字符串，pi TXT 与 /info 用）
    std::string          pkHex;   ///< Ed25519 公钥（小写 hex），pk TXT 与 /info 用
    std::vector<uint8_t> pub;     ///< 32 字节原始公钥
    EVP_PKEY*            priv = nullptr;   ///< Ed25519 私钥（M3 的 pair-verify 用）

    ~DeviceIdentity();
    DeviceIdentity() = default;
    DeviceIdentity(const DeviceIdentity&) = delete;
    DeviceIdentity& operator=(const DeviceIdentity&) = delete;

    /// 从 stateDir 读；不存在则生成并落盘（0600）。失败返回 false 并写 err。
    bool loadOrCreate(const std::string& stateDir, std::string* err);
};

/// 默认 stateDir：$ALLPLAY_STATE_DIR，否则 $HOME/.local/state/allplay
std::string defaultStateDir();

/// 持久化合成 deviceid（MAC 形态，"02:xx:xx:xx:xx:xx"，本地管理位）。
/// 什么时候用：拿不到真实网卡 MAC 时 —— 特别是 **macOS 27 起 getifaddrs 对
/// 非授权进程把链路层地址抹成 02:00:00:00:00:00**（隐私硬化），真实 MAC 又不能
/// 依赖私有 ioctl。合成 id 生成一次后落盘，**跨重启、跨网络稳定**（比会随网络
/// 变化的私有 Wi-Fi 地址更适合当配对标识）。
/// @param created 非空时回填"这次是否新生成"
std::string loadOrCreateSyntheticDeviceId(const std::string& stateDir, bool* created);

}  // namespace core
