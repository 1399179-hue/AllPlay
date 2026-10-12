/**
 * @file   udp.h
 * @brief  UDP socket 抽象（CarPlay 音频走 RTP/RTCP over UDP）
 * @author blt
 */

// ============================================================================
// core/port/udp.h —— UDP socket 抽象（CarPlay 音频走 RTP/RTCP over UDP）
// ============================================================================
// 与 ITcpListener 对称：协议层只吃这个接口，底层由 posix/Android 各自实现。
#pragma once

#include <cstdint>
#include <string>
#include <cstddef>

namespace core {

/// 一个已绑定的 UDP socket（数据报）。
class IUdpSocket {
public:
    virtual ~IUdpSocket() = default;

    /// 绑定本地端口（port=0 由内核选，用 port() 读回）。失败返回 false 并写 err。
    virtual bool bind(uint16_t port, std::string* err) = 0;
    /// 接收一个数据报。>0 = 字节数；0 = 超时；<0 = 已关闭/错误。
    virtual int recv(uint8_t* buf, size_t n, int timeoutMs) = 0;
    /// 把数据报回给最近一次 recv 的来源地址（NTP/RTCP 响应用）。
    virtual bool send(const uint8_t* buf, size_t n) = 0;
    virtual uint16_t port() const = 0;
    virtual void close() = 0;
};

}  // namespace core
