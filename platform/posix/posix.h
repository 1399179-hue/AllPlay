/**
 * @file   posix.h
 * @brief  POSIX 平台适配（Linux + macOS）的装配入口
 * @author blt
 */

// ============================================================================
// platform/posix/posix.h —— POSIX 平台适配（Linux + macOS）的装配入口
// ============================================================================
// core 只依赖 core::PlatformServices；这里提供 POSIX 实现并组装好。
// 各实现文件：net_posix.cpp（TCP/网卡）、mdns_{dns_sd,avahi}.cpp（mDNS）、
// bt_{iobluetooth,bluez}.{mm,cpp}（蓝牙）。桌面组合根（app/main.cpp、tools/*）
// 调用 allplay::posix::makePlatformServices() 注入 core。
#pragma once

#include "port/platform.h"

#include <memory>
#include <string>

namespace allplay::posix {

/// 网卡信息单例（POSIX getifaddrs）
core::INetworkInfo& networkInfo();

/// 各工厂：每用建一个新实例
std::unique_ptr<core::ITcpListener>   makeTcpListener();
std::unique_ptr<core::IUdpSocket>     makeUdpSocket();
std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser();
std::unique_ptr<core::IBluetooth>     makeBluetooth();

/// 组合根辅助：蓝牙控制器地址 / 当前 Wi-Fi SSID（拿不到返回空串）。
/// macOS 27 起两者常被系统打码，同网模式请用 --ssid 显式传。
std::string controllerAddress();
std::string currentSSID();

/// 组装一份完整的 POSIX PlatformServices（network + 三个工厂）。
core::PlatformServices makePlatformServices();

}  // namespace allplay::posix
