/**
 * @file   net_windows.h
 * @brief  Windows（Winsock2）网络适配：TCP/UDP/网卡 + 组合根工厂
 * @author blt
 */

// ============================================================================
// platform/windows/net_windows.h —— Winsock2 适配层声明
// ============================================================================
// 实现 core::ITcpListener / ITcpStream / IUdpSocket / INetworkInfo。
// 读超时用 select()；accept 用 select() 轮询，stop() 关闭 socket 即会唤醒。
#pragma once

#include "port/mdns.h"
#include "port/network.h"
#include "port/platform.h"
#include "port/tcp.h"
#include "port/udp.h"

#include <memory>
#include <string>

namespace allplay::windows {

/// 进程内初始化 Winsock2（幂等）。失败写 err 并返回 false。
bool initWinsock(std::string* err);

core::INetworkInfo& networkInfo();
std::unique_ptr<core::ITcpListener> makeTcpListener();
std::unique_ptr<core::IUdpSocket> makeUdpSocket();
std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser();
core::IBluetooth* makeBluetooth();   // 蓝牙 iAP2 触发（bt_windows.cpp，客户端方向）
core::PlatformServices makePlatformServices();

}  // namespace allplay::windows
