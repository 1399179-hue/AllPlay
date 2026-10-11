/**
 * @file   stream.h
 * @brief  抽象字节流（协议层唯一依赖的"流"形态）
 * @author blt
 */

// ============================================================================
// core/port/stream.h —— 抽象字节流（协议层唯一依赖的"流"形态）
// ============================================================================
// 平台实现：POSIX socket / RFCOMM / 内存管道（测试）… 各自给出 read/write/close。
// core 里的 iAP2、RTSP、屏幕流都只吃这个接口，不关心底层是 socket 还是蓝牙。
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

class IByteStream {
public:
    virtual ~IByteStream() = default;

    /// 读 n 字节。返回 >0 = 实际读到；0 = 超时；<0 = 断开/错误。
    virtual int  read(uint8_t* buf, size_t n, int timeoutMs) = 0;
    /// 全量写（失败返回 false）。
    virtual bool writeAll(const uint8_t* buf, size_t n) = 0;
    /// 主动关闭（默认空操作；平台实现可覆写）。
    virtual void close() {}
};

}  // namespace core
