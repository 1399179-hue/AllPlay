/**
 * @file   statusbar.h
 * @brief  底部状态栏（SDL 自绘，桌面临时 UI）
 * @author blt
 */

// ============================================================================
// statusbar.h —— 底部状态栏（SDL 自绘，桌面临时 UI）
// ============================================================================
//
// 依赖 SDL_ttf + 系统 DejaVu 字体（Ubuntu: fonts-dejavu-core）。
// 拿不到字体就降级为"只画色块不写字"，不影响程序功能 —— 开发机上字体缺失
// 不该让整条链路起不来。
//
// 文案用英文（DejaVu 没有中文字形）。
#pragma once

#include "appstate.h"

#include <SDL.h>
#include <SDL_ttf.h>

#include <string>

namespace allplay {

/// 最小文本渲染器（够状态栏用；不做排版）
class TextRenderer {
public:
    TextRenderer() = default;
    ~TextRenderer();
    TextRenderer(const TextRenderer&) = delete;

    /// pixelHeight: 字号像素高（默认 15）。找不到字体返回 false（不致命）。
    bool init(int pixelHeight = 15);
    bool ok() const { return font_ != nullptr; }
    int  lineHeight() const { return lh_; }

    /// 在 (x, y) 画一行；y 是行顶。
    void draw(SDL_Renderer* r, int x, int y, const std::string& s, SDL_Color c) const;

    /// 量一行文本宽度（字体不可用时按 8px/字符估）
    int measure(const std::string& s) const;

private:
    TTF_Font* font_ = nullptr;
    int       lh_   = 16;
};

/// 底部状态栏：左侧 = 状态点 + 状态文本；右侧 = 分辨率 / fps / 帧数
class StatusBar {
public:
    /// y = 状态栏顶部的窗口坐标；h = 高度
    bool init(SDL_Renderer* r, int winW, int y, int h);

    void draw(SDL_Renderer* r, const SharedStatus& st) const;

    int height() const { return h_; }

private:
    TextRenderer text_;
    SDL_Color    stateColor(LinkState s) const;
    int  y_ = 0, h_ = 24, w_ = 0;
};

}  // namespace allplay
