/**
 * @file   mdns.h
 * @brief  mDNS 广播抽象
 * @author blt
 */

// ============================================================================
// core/port/mdns.h —— mDNS 广播抽象
// ============================================================================
// 平台后端：Linux=avahi-daemon（D-Bus）、macOS=mDNSResponder（dns_sd.h）、
// Android=NsdManager。行为一致：start() 失败不致命（RTSP 照常监听，只是网络上看不到）。
// 发现对端（收 _carplay-ctrl._tcp）属 M3，到时在同层加浏览接口。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace core {

class IMdnsAdvertiser {
public:
    virtual ~IMdnsAdvertiser() = default;

    /// @param name 服务实例名（= AirPlay 列表里显示的名字）
    /// @param port 实际 RTSP 端口
    /// @param txt  "key=value" 列表（deviceid/features/flags/model/srcvers/protovers/pi/pk）
    virtual bool start(const std::string& name, uint16_t port,
                       const std::vector<std::string>& txt, std::string* err) = 0;
    virtual void stop() = 0;
};

}  // namespace core
