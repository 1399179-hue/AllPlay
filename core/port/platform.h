/**
 * @file   platform.h
 * @brief  组合根注入给 core 的平台能力束
 * @author blt
 */

// ============================================================================
// core/port/platform.h —— 组合根注入给 core 的平台能力束
// ============================================================================
// core 只依赖这里的接口 + OpenSSL + C++ stdlib，不出现任何平台头。
// 每个平台的组合根（桌面 app/main.cpp、Android jni_bridge、各 tools）各自装配
// 这些实现并注入。工厂用 std::function 是因为监听器/广播器需要"每用建一个"。
#pragma once

#include "port/bluetooth.h"
#include "port/mdns.h"
#include "port/network.h"
#include "port/tcp.h"
#include "port/udp.h"

#include <functional>
#include <memory>

namespace core {

struct PlatformServices {
    INetworkInfo* network = nullptr;      ///< 网卡 IP/MAC（AirPlay 广播与 0x4301 用）
    IBluetooth*   bluetooth = nullptr;    ///< 蓝牙触发；不触发可为空

    /// 建一个 TCP 监听器（RTSP/数据/事件端口各一个）
    std::function<std::unique_ptr<ITcpListener>()> makeTcpListener;

    /// 建一个 mDNS 广播器（_airplay._tcp）
    std::function<std::unique_ptr<IMdnsAdvertiser>()> makeMdnsAdvertiser;

    /// 建一个 UDP socket（音频 RTP/RTCP）
    std::function<std::unique_ptr<IUdpSocket>()> makeUdpSocket;
};

}  // namespace core
