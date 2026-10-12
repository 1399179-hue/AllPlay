/**
 * @file   launcher.h
 * @brief  启动页（对齐安卓端观感）
 * @author blt
 */

// ============================================================================
// host/launcher.h —— 启动页（对齐安卓端 MainActivity / RippleView 的观感）
// ============================================================================
// 安卓侧的样子（颜色直接取自 MainActivity.kt:123-160 / RippleView.kt:16-22）：
//   白底 → 蓝色圆底 + 白色蓝牙标志 → 两圈扩散水纹 → 双色 "AllPlay" → 状态文字。
// 连接建立后整个启动页让位给全屏视频（见 main.cpp 的 LinkState::Streaming 分支）。
//
// 字体拿不到时不致命：只画图形不写字，程序照跑。
#pragma once

#include "appstate.h"
#include "statusbar.h"

#include <SDL.h>

#include <cstdint>

namespace allplay {

class Launcher {
public:
    /// 建标题字体（粗体无衬线）与状态字体。返回 false = 没字体，只画图形。
    bool init();

    /// 画一帧。nowMs 驱动水纹相位与状态文字后面的省略号。
    void draw(SDL_Renderer* r, int winW, int winH, LinkState st, uint64_t nowMs);

private:
    TextRenderer logo_;     ///< "All" + "Play"
    TextRenderer status_;   ///< 底部状态文字
};

}  // namespace allplay
