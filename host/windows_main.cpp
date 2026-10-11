// ============================================================================
// host/windows_main.cpp —— Windows 桌面宿主（1280×720 Win32 窗口）
// ============================================================================
// 启动 Receiver（RTSP + 屏幕流/音频流网络层），并绘制与安卓一致的启动页：
// 白底 + AllPlay 双色 logo + 蓝色蓝牙徽标(水纹) + 状态文字。
// 视频解码/上屏待接 ffmpeg；当前 video/audio 输出用桩（只统计，不渲染）。
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif

#include "windows/net_windows.h"

#include "airplay/server.h"
#include "common/log.h"
#include "port/media.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace {

constexpr int kWinW = 1280;
constexpr int kWinH = 720;

core::airplay::Receiver* g_receiver = nullptr;
std::atomic<bool>       g_running{true};

// 水纹动画进度 0..1（定时器驱动）
double g_ripple = 0.0;

// ---------------------------------------------------------------------------
// 视频/音频桩：让屏幕流(TCP)、音频流(UDP)真正建立，但不解码渲染
// ---------------------------------------------------------------------------
class StubVideo : public core::IVideoOutput {
public:
    void configure(core::VideoCodec, int w, int h, const std::vector<uint8_t>&) override
    {
        LOG_INFO("视频输出桩：配置 %dx%d", w, h);
    }
    bool feedAnnexB(const uint8_t*, size_t n) override
    {
        static std::atomic<uint64_t> frames{0};
        const uint64_t f = ++frames;
        if (f == 1 || f % 300 == 0) LOG_INFO("视频输出桩：第 %llu 帧 %zu 字节", (unsigned long long)f, n);
        return true;
    }
    void reset() override {}
    void flush() override {}
};

class StubAudio : public core::IAudioOutput {
public:
    void configure(core::AudioCodec c, int rate, int ch) override
    {
        LOG_INFO("音频输出桩：配置 codec=%d %dHz %dch", int(c), rate, ch);
    }
    void feed(const uint8_t*, size_t n, uint32_t) override
    {
        static std::atomic<uint64_t> pkts{0};
        const uint64_t p = ++pkts;
        if (p == 1 || p % 500 == 0) LOG_INFO("音频输出桩：第 %llu 包 %zu 字节", (unsigned long long)p, n);
    }
    void reset() override {}
};

StubVideo g_video;
StubAudio g_audio;

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
void drawCenteredText(HDC dc, const std::string& text, int cx, int cy, COLORREF color,
                      int height, int weight)
{
    HFONT f = CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
    HFONT old = static_cast<HFONT>(SelectObject(dc, f));
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    RECT r{cx - 300, cy - height, cx + 300, cy + height};
    DrawTextA(dc, text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    DeleteObject(f);
}

void drawLauncher(HDC dc)
{
    // 白底
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    RECT full{0, 0, kWinW, kWinH};
    FillRect(dc, &full, white);
    DeleteObject(white);

    const int cx = kWinW / 2;
    const int titleY = 180;
    const int badgeCx = cx;
    const int badgeCy = 360;
    const int badgeR = 42;

    // AllPlay 双色 logo：先画 "All" 深色，再画 "Play" 蓝色（简单起见用两次 TextOut）
    drawCenteredText(dc, "AllPlay", cx, titleY, RGB(30, 30, 30), 40, FW_BOLD);

    // 水纹环（两圈，扩散渐隐）
    HPEN ringPen = CreatePen(PS_SOLID, 3, RGB(47, 124, 246));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, ringPen));
    HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, hollow));
    for (int i = 0; i < 2; ++i) {
        double p = g_ripple + i * 0.5;
        p -= std::floor(p);   // 0..1
        int r = badgeR + int(p * (badgeR * 2));
        Ellipse(dc, badgeCx - r, badgeCy - r, badgeCx + r, badgeCy + r);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(ringPen);

    // 蓝色圆底 + 白色蓝牙符号
    HBRUSH blue = CreateSolidBrush(RGB(47, 124, 246));
    oldBrush = static_cast<HBRUSH>(SelectObject(dc, blue));
    HPEN noPen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen2 = static_cast<HPEN>(SelectObject(dc, noPen));
    Ellipse(dc, badgeCx - badgeR, badgeCy - badgeR, badgeCx + badgeR, badgeCy + badgeR);
    SelectObject(dc, oldPen2);
    SelectObject(dc, oldBrush);
    DeleteObject(noPen);
    DeleteObject(blue);
    drawCenteredText(dc, "B", badgeCx, badgeCy, RGB(255, 255, 255), 36, FW_BOLD);

    // 状态文字
    std::string status = "等待连接";
    if (g_receiver && g_receiver->sessionActive()) status = "已连接";
    else if (g_receiver && g_receiver->connecting()) status = "连接中…";
    drawCenteredText(dc, status, cx, 470, RGB(102, 102, 102), 20, FW_NORMAL);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        drawLauncher(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {           // 水纹动画
            g_ripple += 0.016;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == 2) {    // 状态刷新
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int main()
{
    std::string err;
    if (!allplay::windows::initWinsock(&err)) {
        LOG_ERROR("Winsock 初始化失败：%s", err.c_str());
        return 1;
    }

    // 组合根：Windows 平台能力
    auto services = allplay::windows::makePlatformServices();

    core::airplay::ReceiverConfig cfg;
    cfg.name      = "AllPlay";
    cfg.width     = 1280;
    cfg.height    = 720;
    cfg.fps       = 60;
    cfg.stateDir  = ".";
    cfg.mfiDir    = "identity";      // 相对 cwd；可改成绝对路径
    cfg.btMac     = "";
    cfg.video     = &g_video;
    cfg.audio     = &g_audio;
    cfg.enableAvahi = false;         // mDNS 桩，关掉避免噪音

    core::airplay::Receiver receiver;
    if (!receiver.start(cfg, services, &err)) {
        LOG_ERROR("Receiver 启动失败：%s", err.c_str());
        return 1;
    }
    g_receiver = &receiver;
    LOG_INFO("AllPlay Windows 宿主已启动：RTSP 端口 %u，IP %s", receiver.port(), receiver.ip().c_str());

    // 窗口
    WNDCLASSW wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AllPlayHost";
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    RegisterClassW(&wc);

    RECT rc{0, 0, kWinW, kWinH};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"AllPlayHost", L"AllPlay", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { LOG_ERROR("窗口创建失败"); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    SetTimer(hwnd, 1, 33, nullptr);    // 水纹 ~30fps
    SetTimer(hwnd, 2, 500, nullptr);   // 状态 500ms

    // 消息循环
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g_running = false;
    receiver.stop();
    return 0;
}
