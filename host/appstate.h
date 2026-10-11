/**
 * @file   appstate.h
 * @brief  跨线程共享的展示状态
 * @author blt
 */

// ============================================================================
// appstate.h —— 跨线程共享的展示状态
// ============================================================================
//
// 线程约定（跟 Plan 的宿主设计一致）：
//   · 核心 / 蓝牙 / 解码线程：写（都用 atomic；errorMsg 只在切到 Error 时写）
//   · 主线程（SDL 渲染）：读
// 主线程永远不阻塞在锁上 —— 除了 errorMsg 那条很短的互斥。
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace allplay {

/// 连接状态（状态栏显示 + 后续画面归属切换的判据）
enum class LinkState : int {
    Idle = 0,        ///< 刚启动
    WaitingPair,     ///< 等蓝牙配对/触发（M2 起由 bt 层驱动）
    Connecting,      ///< 已触发，等 AirPlay 会话建立
    Streaming,       ///< 会话活跃，视频在流
    Error,           ///< 出错，见 errorMsg
};

inline const char* toString(LinkState s)
{
    switch (s) {
        case LinkState::Idle:        return "IDLE";
        case LinkState::WaitingPair: return "WAIT-BT";
        case LinkState::Connecting:  return "CONNECTING";
        case LinkState::Streaming:   return "STREAMING";
        case LinkState::Error:       return "ERROR";
    }
    return "?";
}

struct SharedStatus {
    std::atomic<int>       state{static_cast<int>(LinkState::Idle)};
    std::atomic<int>       videoW{0};        ///< 当前视频宽（0 = 未知）
    std::atomic<int>       videoH{0};        ///< 当前视频高
    std::atomic<int>       fps{0};           ///< 解码显示帧率（1 秒窗口）
    std::atomic<uint64_t>  frames{0};        ///< 累计显示帧数
    std::atomic<uint64_t>  lastFrameMs{0};   ///< 最近一帧的墙钟毫秒（0 = 还没有过）

    // 只在 state 切到 Error 时由写方更新
    void setError(const std::string& msg)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            errorMsg_ = msg;
        }
        state.store(static_cast<int>(LinkState::Error), std::memory_order_release);
    }

    std::string error() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return errorMsg_;
    }

private:
    mutable std::mutex mu_;
    std::string        errorMsg_;
};

}  // namespace allplay
