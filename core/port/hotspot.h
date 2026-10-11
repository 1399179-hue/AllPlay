/**
 * @file   hotspot.h
 * @brief  无线热点抽象（热点模式）
 * @author blt
 */

// ============================================================================
// core/port/hotspot.h —— 无线热点抽象（热点模式）
// ============================================================================
// 热点模式：配件自己开一个本地热点，iPhone 连进这个热点，两者直连、不经路由器。
// 平台实现：Android = WifiManager.startLocalOnlyHotspot（LocalOnlyHotspotManager）；
//           桌面 = 桩（Linux create_ap / NetworkManager 后续接）。
// core 不直接持有热点，只把它的 ssid/passphrase/ip 填进 WirelessTriggerConfig。
#pragma once

#include <string>

namespace core {

class IHotspot {
public:
    virtual ~IHotspot() = default;

    /// 开热点（阻塞到就绪或失败）。失败返回 false 并写 err。
    virtual bool start(std::string* err) = 0;
    virtual void stop() = 0;

    virtual const std::string& ssid() const = 0;        ///< 热点名（给 iPhone 的 0x5703）
    virtual const std::string& passphrase() const = 0;  ///< 热点口令（给 iPhone 的 0x5703）
    virtual const std::string& ip() const = 0;          ///< 热点接口的 IPv4（给 0x4301 的 ipAddresses）
};

}  // namespace core
