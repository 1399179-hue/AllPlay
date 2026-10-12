// ============================================================================
// video_probe.cpp —— 本地喂 H.264 冒烟：验证"解码 → letterbox 上屏"这条链路
// ============================================================================
// 不碰协议、不碰网络、不碰蓝牙：把一段裸 AnnexB H.264 直接喂进 SdlVideoSink，
// 肉眼确认解码正常、宽高比正确、帧率跟得上。
//
// 它是排查"黑屏"的**二分点**（这是它最大的价值）：
//   有画面 → 问题在协议侧（RTSP / 加密 / 分帧 / 配置帧顺序）
//   没画面 → 问题在解码侧（extradata / 像素格式 / 纹理）
//
// 造测试流（任意带 H.264 的容器都行）：
//   ffmpeg -i in.mp4 -c:v libx264 -bsf:v h264_mp4toannexb -an out.h264
//
// 用法：
//   video_probe --h264 out.h264 [--fps 30] [--loop] [--reset-at 60] [--frames N]
#include "annexb.h"
#include "media/sdl_video_sink.h"
#include "screenshot.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// AnnexB 解析在 tools/annexb.h —— 与假手机共用，保证两边对码流的解释一致
// （"probe 有画面但真机黑屏"这条二分线只有在两边同源时才说明得了问题）。
using allplay::annexb::buildAvcC;
using allplay::annexb::NalRef;
using allplay::annexb::scanNals;
using allplay::annexb::splitAus;

namespace {

struct Options {
    std::string h264;              // --h264，必填（裸 AnnexB 基本流）
    int         fps      = 30;
    bool        loop     = false;
    int         resetAt  = 0;      // >0：在第 N 个 AU 处从另一线程调 sink.reset()
    int         frames   = 0;      // >0：渲染 N 帧后退出（容器冒烟用）
    int         winW     = 1280;
    int         winH     = 720;
    std::string dump;              // --dump，退出前把最后一帧渲染结果存成 PPM
    bool        help     = false;
};

void printUsage(const char* argv0)
{
    std::printf(
        "用法: %s --h264 FILE [选项]\n"
        "  --h264 PATH       裸 AnnexB H.264 基本流（必需）\n"
        "  --fps N           按 N 帧/秒喂入（默认 30）\n"
        "  --loop            放完从头再来\n"
        "  --reset-at N      第 N 个访问单元处从另一线程调 reset()，验证关键帧恢复\n"
        "  --frames N        渲染 N 帧后退出（无头冒烟，如 SDL_VIDEODRIVER=dummy）\n"
        "  --window WxH      窗口尺寸（默认 1280x720）\n"
        "  --dump PATH       退出前把最后一帧的渲染结果存成 PPM（机器校验颜色用）\n"
        "  -h, --help        显示本帮助\n"
        "\n"
        "退出：窗口关闭 / ESC / 播完（未加 --loop）。\n",
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
        if (!std::strcmp(a, "--h264")) {
            o->h264 = need(a);
        } else if (!std::strcmp(a, "--fps")) {
            o->fps = std::atoi(need(a));
            if (o->fps <= 0 || o->fps > 240) {
                std::fprintf(stderr, "--fps 超范围（1..240）\n");
                return false;
            }
        } else if (!std::strcmp(a, "--loop")) {
            o->loop = true;
        } else if (!std::strcmp(a, "--reset-at")) {
            o->resetAt = std::atoi(need(a));
        } else if (!std::strcmp(a, "--frames")) {
            o->frames = std::atoi(need(a));
        } else if (!std::strcmp(a, "--window")) {
            if (!parseScreen(need(a), &o->winW, &o->winH)) {
                std::fprintf(stderr, "--window 需要 WxH，如 1280x720\n");
                return false;
            }
        } else if (!std::strcmp(a, "--dump")) {
            o->dump = need(a);
        } else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
            o->help = true;
        } else {
            std::fprintf(stderr, "未知参数：%s（-h 看帮助）\n", a);
            return false;
        }
    }
    if (!o->help && o->h264.empty()) {
        std::fprintf(stderr, "必须给 --h264 FILE（-h 看帮助）\n");
        return false;
    }
    return true;
}

bool readFile(const std::string& path, std::vector<uint8_t>* out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "打不开 %s\n", path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fprintf(stderr, "%s 是空文件\n", path.c_str());
        std::fclose(f);
        return false;
    }
    out->resize(size_t(n));
    const size_t got = std::fread(out->data(), 1, size_t(n), f);
    std::fclose(f);
    if (got != size_t(n)) {
        std::fprintf(stderr, "读 %s 只拿到 %zu/%ld 字节\n", path.c_str(), got, n);
        return false;
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

    std::vector<uint8_t> buf;
    if (!readFile(opt.h264, &buf)) return 1;

    const auto nals = scanNals(buf);
    const auto aus  = splitAus(buf, nals);
    if (aus.empty()) {
        std::fprintf(stderr, "在 %s 里没找到任何起始码 —— 不是 AnnexB 流？\n", opt.h264.c_str());
        return 1;
    }

    std::vector<uint8_t> avcC;
    std::string err;
    if (!buildAvcC(buf, nals, &avcC, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("[probe] %s：%zu 字节，%zu 个 NAL，%zu 个访问单元，avcC %zu 字节\n",
                opt.h264.c_str(), buf.size(), nals.size(), aus.size(), avcC.size());

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "[probe] SDL_Init 失败：%s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("AllPlay video_probe",
                                       SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       opt.winW, opt.winH, SDL_WINDOW_RESIZABLE);
    if (!win) {
        std::fprintf(stderr, "[probe] 建窗口失败：%s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Renderer* ren = SDL_CreateRenderer(
        win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    bool vsync = true;
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        vsync = false;
    }
    if (!ren) {
        std::fprintf(stderr, "[probe] 建渲染器失败：%s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    allplay::SdlVideoSink sink;
    // width/height 只是协商值，解码器不依赖它（真实尺寸看 SPS）
    sink.configure(core::VideoCodec::H264, opt.winW, opt.winH, avcC);

    const double frameMs = 1000.0 / double(opt.fps);
    Uint64       t0      = SDL_GetTicks64();
    Uint64       base    = t0;
    size_t       au      = 0;
    size_t       fedTotal = 0;
    bool         fedAll  = false;
    Uint64       doneAt  = 0;
    Uint64       feedEnd = 0;   ///< 喂完那一刻（算解码速率用，不含收尾的展示时间）
    bool         running  = true;
    bool         reported = false;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
        }

        // —— 按帧率节奏喂帧 ——
        const Uint64 now = SDL_GetTicks64();
        while (!fedAll && now >= base + Uint64(double(au) * frameMs)) {
            if (opt.resetAt > 0 && int(au) == opt.resetAt) {
                std::printf("[probe] 第 %zu 个 AU：从另一线程调 reset()（模拟 RTSP 线程的关键帧请求）\n", au);
                std::thread([&sink] { sink.reset(); }).join();
            }
            const auto& r = aus[au];
            sink.feedAnnexB(buf.data() + r.first, r.second - r.first);
            ++fedTotal;
            if (opt.frames > 0 && fedTotal >= size_t(opt.frames) && feedEnd == 0) feedEnd = now;
            if (++au >= aus.size()) {
                fedAll = true;
                doneAt = now;
                if (opt.loop) {
                    au = 0;
                    base = now;
                    fedAll = false;
                }
            }
        }
        // 喂完且不在循环 → 再留 400ms 把最后一帧显示完就退
        if (fedAll && now - doneAt > 400) running = false;

        // —— 上屏 ——
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        int vw = 0, vh = 0;
        sink.intrinsicSize(&vw, &vh);
        int winW = 0, winH = 0;
        SDL_GetRendererOutputSize(ren, &winW, &winH);
        SDL_Rect dst;
        allplay::fitLetterbox(vw, vh, winW, winH, &dst);
        sink.present(ren, dst);
        SDL_RenderPresent(ren);

        if (!reported && vw > 0 && vh > 0) {
            std::printf("[probe] 解出首帧：%dx%d（letterbox 落点 %d,%d %dx%d）\n",
                        vw, vh, dst.x, dst.y, dst.w, dst.h);
            reported = true;
        }

        // --frames 数的是**喂进去的视频帧**，不是渲染循环的圈数 —— 后者会随渲染后端
        // （vsync 与否）变，冒烟测试会在软件渲染的机器上悄悄只跑几帧就"通过"。
        if (opt.frames > 0 && fedTotal >= size_t(opt.frames)) running = false;
        if (!vsync) SDL_Delay(2);   // 软件/dummy 渲染器 —— 别把 CPU 空转满
    }

    if (!opt.dump.empty()) allplay::dumpRender(ren, opt.dump);

    int fw = 0, fh = 0;
    sink.intrinsicSize(&fw, &fh);
    const Uint64 elapsed = SDL_GetTicks64() - t0;
    std::printf("[probe] 共喂 %zu/%zu 个 AU：解码 %llu 帧、浅队列挤掉 %llu 帧、上屏 %llu 帧，%s，耗时 %llu ms\n",
                au, aus.size(), (unsigned long long)sink.decoded(),
                (unsigned long long)sink.dropped(),
                (unsigned long long)sink.presented(),
                sink.configured() ? "解码器正常" : "解码器未配置",
                (unsigned long long)elapsed);
    // 性能判定看**解码速率**（喂帧侧的耗时），不看上屏 fps：
    // 上屏速率受渲染后端牵制（无头/软件渲染时慢得离谱），拿它判"这颗 CPU 能不能软解"
    // 会把渲染的锅算到解码头上。挤掉的帧是浅队列的设计行为，不是解码失败。
    const Uint64 feedMs = feedEnd > t0 ? feedEnd - t0 : elapsed;
    if (feedMs > 0) {
        std::printf("[probe] 解码 %.1f fps（喂完 %zu 帧用了 %llu ms）；上屏 %.1f fps\n",
                    double(sink.decoded()) * 1000.0 / double(feedMs),
                    fedTotal, (unsigned long long)feedMs,
                    double(sink.presented()) * 1000.0 / double(elapsed));
    }
    if (fw == 0) std::printf("[probe] ⚠️ 一帧都没解出来（%dx%d）—— 看看上面有没有解码器报错\n", fw, fh);

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return sink.presented() > 0 ? 0 : 1;
}
