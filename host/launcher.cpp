#include "launcher.h"

#include <cmath>
#include <string>

namespace allplay {

namespace {

// 颜色取自安卓侧（MainActivity.kt / RippleView.kt），别随手改
constexpr SDL_Color kInk    = {0x1A, 0x1A, 0x1A, 255};   // "All"
constexpr SDL_Color kBlue   = {0x2F, 0x7C, 0xF6, 255};   // "Play" / 圆底 / 水纹
constexpr SDL_Color kGray   = {0x66, 0x66, 0x66, 255};   // 状态文字

constexpr double kRipplePeriodMs = 2800.0;   // RippleView.kt:42-52 的周期
constexpr int    kRippleCount    = 2;        // 两圈，相位错开 0.5

/// SDL2 没有画圆 API，自己按扫描线画。
void fillCircle(SDL_Renderer* r, int cx, int cy, int rad)
{
    for (int dy = -rad; dy <= rad; ++dy) {
        const int dx = int(std::sqrt(double(rad * rad - dy * dy)));
        SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

/// 圆环：|dy| >= 内半径时整行都是环；否则左右各画一段弧。
void strokeRing(SDL_Renderer* r, int cx, int cy, int rad, int thick)
{
    const int ri = rad - thick;
    if (ri <= 0) {
        fillCircle(r, cx, cy, rad);
        return;
    }
    for (int dy = -rad; dy <= rad; ++dy) {
        const int xo = int(std::sqrt(double(rad * rad - dy * dy)));
        if (std::abs(dy) >= ri) {
            SDL_RenderDrawLine(r, cx - xo, cy + dy, cx + xo, cy + dy);
        } else {
            const int xi = int(std::sqrt(double(ri * ri - dy * dy)));
            SDL_RenderDrawLine(r, cx - xo, cy + dy, cx - xi, cy + dy);
            SDL_RenderDrawLine(r, cx + xi, cy + dy, cx + xo, cy + dy);
        }
    }
}

/// 蓝牙符文：一条竖脊 + 上下两个朝右的折角（读起来像个 B）。
/// 比画个字母 "B" 像样，而且不依赖字体。
void drawBluetoothRune(SDL_Renderer* r, int cx, int cy, int rad)
{
    struct P { double x, y; };
    const P spineT{0.0, -1.0}, spineB{0.0, 1.0};
    const P mid{0.0, 0.0};
    const P upR{0.68, -0.34}, loR{0.68, 0.34};

    auto line = [&](P a, P b) {
        SDL_RenderDrawLine(r, cx + int(a.x * rad), cy + int(a.y * rad),
                              cx + int(b.x * rad), cy + int(b.y * rad));
    };
    line(spineT, spineB);   // 竖脊
    line(spineT, upR);      // 上折角
    line(upR, mid);
    line(mid, loR);         // 下折角
    line(loR, spineB);
}

/// 状态文字（含随时间的省略号动画）
std::string statusText(LinkState st, uint64_t nowMs)
{
    switch (st) {
        case LinkState::Idle:
        case LinkState::WaitingPair: {
            std::string s = "Waiting for iPhone";
            s += std::string(size_t((nowMs / 700) % 4), '.');   // "", ".", "..", "..."
            return s;
        }
        case LinkState::Connecting:
            return "Connecting" + std::string(size_t((nowMs / 400) % 4), '.');
        case LinkState::Streaming:
            return "Connected";
        case LinkState::Error:
            return "Error";
    }
    return "";
}

}  // namespace

bool Launcher::init()
{
    const bool a = logo_.init(44, FontFace::Sans, /*bold=*/true);
    const bool b = status_.init(19, FontFace::Sans);
    return a && b;
}

void Launcher::draw(SDL_Renderer* r, int winW, int winH, LinkState st, uint64_t nowMs)
{
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);   // 白底
    SDL_RenderClear(r);

    // 一切尺寸按短边缩放，小窗口也不会糊成一团
    const int    ref  = (winW < winH ? winW : winH);
    const int    cx   = winW / 2;
    const int    badgeY = int(winH * 0.36);
    const int    badgeR = int(ref * 0.085);
    const int    maxR   = int(ref * 0.30);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);   // 水纹要按 alpha 渐隐

    // —— 水纹（画在圆底之前，让圆底盖住内向的部分）——
    const double phase = std::fmod(double(nowMs) / kRipplePeriodMs, 1.0);
    for (int i = 0; i < kRippleCount; ++i) {
        const double p = std::fmod(phase + double(i) / kRippleCount, 1.0);
        const int    rad = badgeR + int(p * (maxR - badgeR));
        if (rad <= badgeR) continue;
        const Uint8 alpha = Uint8((1.0 - p) * 160.0);
        SDL_SetRenderDrawColor(r, kBlue.r, kBlue.g, kBlue.b, alpha);
        strokeRing(r, cx, badgeY, rad, 3);
    }

    // —— 蓝色圆底 + 白色蓝牙符文 ——
    SDL_SetRenderDrawColor(r, kBlue.r, kBlue.g, kBlue.b, 255);
    fillCircle(r, cx, badgeY, badgeR);

    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    for (int w = -1; w <= 1; ++w) {          // 描 3 遍凑粗，SDL 线宽不可调
        drawBluetoothRune(r, cx + w, badgeY, int(badgeR * 0.52));
    }

    // —— 双色 logo：两段分别 draw，拼成 "AllPlay" ——
    if (logo_.ok()) {
        const std::string a = "All", b = "Play";
        const int wa = logo_.measure(a), wb = logo_.measure(b);
        const int x0 = (winW - (wa + wb)) / 2;
        const int y0 = int(winH * 0.56);
        logo_.draw(r, x0,      y0, a, kInk);
        logo_.draw(r, x0 + wa, y0, b, kBlue);
    }

    // —— 状态文字 ——
    if (status_.ok()) {
        const std::string s = statusText(st, nowMs);
        status_.draw(r, (winW - status_.measure(s)) / 2, int(winH * 0.72), s, kGray);
    }

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

}  // namespace allplay
