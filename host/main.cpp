// ============================================================================
// main.cpp —— Ubuntu 无线 CarPlay 接收端宿主（M0：窗口 + 状态栏骨架）
// ============================================================================
//
// 这个进程就是"车机"侧宿主：后续里程碑会在这里接上
//   M1: 核心（AirPlay 协议实现）——Bonjour/RTSP/会话
//   M2: 蓝牙触发（RFCOMM + iAP2 bootstrap）
//   M3: 视频（解码缝 → ffmpeg → SDL 纹理）
//   M5: LVGL UI 叠加（BoxUI 框架）
// 线程模型：主线程独占 SDL（事件+渲染）；核心/蓝牙/解码各在自己的线程，
// 只通过 SharedStatus（atomic）与（M3 起的）帧队列跟主线程交互。
#include "appstate.h"
#include "statusbar.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct Options {
    int         screenW  = 1280;
    int         screenH  = 720;
    int         barH     = 26;
    bool        fullscreen = false;
    int         frames   = 0;      // >0：渲染 N 帧后退出（容器冒烟用）
    std::string font;              // --font，可空（空则用候选表）
    bool        help     = false;
};

void printUsage(const char* argv0)
{
    std::printf(
        "用法: %s [选项]\n"
        "  --screen WxH      视频面尺寸（默认 1280x720，也是向手机协商的目标分辨率）\n"
        "  --fullscreen      全屏（SDL_WINDOW_FULLSCREEN_DESKTOP）\n"
        "  --statusbar-h N   状态栏高度（默认 26）\n"
        "  --font PATH       状态栏字体（默认 DejaVu；可用 ALLPLAY_FONT 环境变量）\n"
        "  --frames N        渲染 N 帧后退出（无头冒烟测试用，如 SDL_VIDEODRIVER=dummy）\n"
        "  -h, --help        显示本帮助\n"
        "\n"
        "退出：窗口关闭 / ESC / Ctrl-C。\n",
        argv0);
}

bool parseScreen(const char* s, int* w, int* h)
{
    return std::sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

bool parseArgs(int argc, char** argv, Options* o)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "参数 %s 缺少取值\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--screen")) {
            if (!parseScreen(need(a), &o->screenW, &o->screenH)) {
                std::fprintf(stderr, "--screen 需要 WxH，如 1280x720\n");
                return false;
            }
        } else if (!std::strcmp(a, "--fullscreen")) {
            o->fullscreen = true;
        } else if (!std::strcmp(a, "--statusbar-h")) {
            o->barH = std::atoi(need(a));
            if (o->barH < 10 || o->barH > 200) {
                std::fprintf(stderr, "--statusbar-h 超范围（10..200）\n");
                return false;
            }
        } else if (!std::strcmp(a, "--font")) {
            o->font = need(a);
        } else if (!std::strcmp(a, "--frames")) {
            o->frames = std::atoi(need(a));
        } else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
            o->help = true;
        } else {
            std::fprintf(stderr, "未知参数：%s（-h 看帮助）\n", a);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!parseArgs(argc, argv, &opt)) return 2;
    if (opt.help) {
        printUsage(argv[0]);
        return 0;
    }
    if (!opt.font.empty()) setenv("ALLPLAY_FONT", opt.font.c_str(), 1);

    std::printf("[host] allplay_host (M0) — 视频面 %dx%d，状态栏 %dpx%s\n",
                opt.screenW, opt.screenH, opt.barH,
                opt.fullscreen ? "，全屏" : "");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "[host] SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }

    Uint32 winFlags = SDL_WINDOW_RESIZABLE;
    if (opt.fullscreen) winFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_Window* win = SDL_CreateWindow(
        "AllPlay", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        opt.screenW, opt.screenH + opt.barH, winFlags);
    if (!win) {
        std::fprintf(stderr, "[host] 建窗口失败：%s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // 优先硬件渲染 + vsync；无显示环境（SDL_VIDEODRIVER=dummy 冒烟）回落软件
    SDL_Renderer* ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    bool vsync = true;
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        vsync = false;
    }
    if (!ren) {
        std::fprintf(stderr, "[host] 建渲染器失败：%s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    allplay::SharedStatus status;
    // M0：还没有蓝牙/核心，直接停在"等配对"这个名义状态（M2 起由 bt 层驱动）
    status.state.store(static_cast<int>(allplay::LinkState::WaitingPair));

    allplay::StatusBar bar;
    bar.init(ren, opt.screenW, opt.screenH, opt.barH);

    bool running = true;
    int  rendered = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        // 视频区（M3 起贴视频纹理；现在纯黑）
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);

        bar.draw(ren, status);

        SDL_RenderPresent(ren);

        if (opt.frames > 0 && ++rendered >= opt.frames) {
            std::printf("[host] 冒烟：已渲染 %d 帧，正常退出\n", rendered);
            break;
        }
        if (!vsync) SDL_Delay(16);   // 软件渲染时按 ~60fps 走
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
