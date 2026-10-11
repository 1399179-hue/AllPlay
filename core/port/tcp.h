/**
 * @file   tcp.h
 * @brief  TCP 监听/连接抽象
 * @author blt
 */

// ============================================================================
// core/port/tcp.h —— TCP 监听/连接抽象
// ============================================================================
// RTSP 控制端口、屏幕流数据端口、事件端口都建在它上面。读超时由平台层保证
// （POSIX=SO_RCVTIMEO/poll，Windows=SO_RCVTIMEO，Android=Socket timeout）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace core {

/// 一条已建立的 TCP 连接（字节流）。
class ITcpStream {
public:
    virtual ~ITcpStream() = default;
    virtual int  read(uint8_t* buf, size_t n, int timeoutMs) = 0;  // >0 n；0 超时；<0 断开
    virtual bool writeAll(const uint8_t* buf, size_t n) = 0;
    virtual void close() = 0;
};

/// TCP 监听器。
class ITcpListener {
public:
    virtual ~ITcpListener() = default;

    /// 绑定并监听。port=0 时由内核选端口（用 port() 读回）。失败返回 false 并写 err。
    virtual bool start(const std::string& bindAddr, uint16_t port, std::string* err) = 0;
    /// 阻塞 accept 一条连接（timeoutMs<=0 永不超时）。超时/已停止返回 nullptr。
    virtual std::unique_ptr<ITcpStream> accept(int timeoutMs) = 0;
    virtual uint16_t port() const = 0;
    /// 停止并唤醒 accept()。
    virtual void stop() = 0;
};

}  // namespace core
