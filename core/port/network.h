/**
 * @file   network.h
 * @brief  网卡信息抽象（IP/MAC 枚举）
 * @author blt
 */

// ============================================================================
// core/port/network.h —— 网卡信息抽象（IP/MAC 枚举）
// ============================================================================
// 平台实现：POSIX getifaddrs（+ioctl 兜底）/ Android NetworkInterface / Windows。
// 同网模式的重点是"挑对接口"：Bonjour 广播与 0x4301 给手机的 IP 必须是手机能到达的。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace core {

class INetworkInfo {
public:
    virtual ~INetworkInfo() = default;

    /// 自动挑一个"能到手机"的接口：第一个 up、非 loopback、有 IPv4 的。
    /// @param ifnameOut 实际选中接口名（可为空串 = 没找到）
    /// @return 接口 IPv4（点分十进制）；没找到返回空串
    virtual std::string pickInterfaceIPv4(std::string* ifnameOut) = 0;

    /// 指定接口的 IPv4（不存在/没地址返回空串）
    virtual std::string interfaceIPv4(const std::string& ifname) = 0;

    /// 指定接口的 MAC（6 字节）。成功返回 true。
    virtual bool interfaceMAC(const std::string& ifname, uint8_t mac[6]) = 0;

    // ---- 有线 CarPlay 的媒体面（内核 cdc_ncm）---------------------------------
    // 有线这条道上手机不是连我们的 Wi-Fi，而是走 USB 上的 NCM 以太网。那张网卡
    // 是**内核驱动挂出来的**，名字（enx…）不固定，得按 USB 设备反查。

    /// 按 USB 设备的 bus/addr 找到它挂出来的网卡名。找不到返回空串。
    /// 默认实现返回空 —— 平台没有这条路径（Android 走 VpnService，不是内核 NCM）。
    virtual std::string interfaceForUsbDevice(int bus, int address)
    {
        (void)bus;
        (void)address;
        return {};
    }

    /// 指定接口的 IPv6 链路本地地址（不带 %zone 后缀）。没有返回空表。
    /// 有线 0x4301 报给手机的就是它 —— DiPlay 那条路上等价于 config.linkLocal。
    virtual std::vector<std::string> interfaceIPv6LinkLocal(const std::string& ifname)
    {
        (void)ifname;
        return {};
    }
};

/// MAC 6 字节 → "AA:BB:CC:DD:EE:FF"（纯格式，平台无关）
inline std::string deviceIdFromMAC(const uint8_t mac[6])
{
    char buf[18];
    std::snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

}  // namespace core
