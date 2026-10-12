#include "statusbar.h"

#include <cstdio>
#include <cstdlib>

namespace allplay {

namespace {

// 候选字体（第一个能开的就用）。可用 ALLPLAY_FONT 环境变量覆盖。
const char* const kMonoCandidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
};

const char* const kSansCandidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
};

// 粗体优先用真粗体字面（比 TTF 的合成粗体好看）；拿不到再退到合成
const char* const kBoldCandidates[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
};

// TTF_Init/TTF_Quit 是库级全局状态：多个 TextRenderer 共存时必须引用计数，
// 否则先析构的那个会把还活着的那些字体一起带走。
int g_ttfRefs = 0;

void drawText(SDL_Renderer* r, TTF_Font* font, int x, int y,
              const std::string& s, SDL_Color c)
{
    if (!font || s.empty()) return;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(font, s.c_str(), c);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    if (tex) {
        SDL_Rect dst{x, y, surf->w, surf->h};
        SDL_RenderCopy(r, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}

}  // namespace

// ---------------------------------------------------------------------------
// TextRenderer
// ---------------------------------------------------------------------------

TextRenderer::~TextRenderer()
{
    if (font_) TTF_CloseFont(font_);
    if (g_ttfRefs > 0 && --g_ttfRefs == 0) TTF_Quit();
}

bool TextRenderer::init(int pixelHeight, FontFace face, bool bold)
{
    if (!font_) {
        if (g_ttfRefs == 0 && TTF_Init() != 0) {
            std::fprintf(stderr, "[statusbar] TTF_Init 失败：%s（状态栏不显示文本）\n",
                         TTF_GetError());
            return false;
        }
        ++g_ttfRefs;
    }

    const char* env = std::getenv("ALLPLAY_FONT");
    if (env && *env) {
        font_ = TTF_OpenFont(env, pixelHeight);
        if (!font_)
            std::fprintf(stderr, "[statusbar] ALLPLAY_FONT=\"%s\" 打不开：%s\n",
                         env, TTF_GetError());
    }
    // 粗体先试真粗体字面，再退到当前族
    if (bold) {
        for (const char* p : kBoldCandidates) {
            if (font_) break;
            font_ = TTF_OpenFont(p, pixelHeight);
            if (font_) hasRealBold_ = true;
        }
    }
    for (const char* p : (face == FontFace::Sans ? kSansCandidates : kMonoCandidates)) {
        if (font_) break;
        font_ = TTF_OpenFont(p, pixelHeight);
    }
    if (!font_) {
        std::fprintf(stderr, "[statusbar] 找不到可用字体（装有 fonts-dejavu-core 吗？）"
                             "—— 状态栏降级为色块\n");
        return false;
    }
    if (bold && !hasRealBold_) TTF_SetFontStyle(font_, TTF_STYLE_BOLD);
    lh_ = TTF_FontLineSkip(font_);
    return true;
}

void TextRenderer::draw(SDL_Renderer* r, int x, int y, const std::string& s,
                        SDL_Color c) const
{
    drawText(r, font_, x, y, s, c);
}

int TextRenderer::measure(const std::string& s) const
{
    if (!font_) return static_cast<int>(s.size()) * 8;
    int w = 0, h = 0;
    TTF_SizeUTF8(font_, s.c_str(), &w, &h);
    return w;
}

// ---------------------------------------------------------------------------
// StatusBar
// ---------------------------------------------------------------------------

bool StatusBar::init(SDL_Renderer* /*r*/, int winW, int y, int h)
{
    w_ = winW;
    y_ = y;
    h_ = h;
    text_.init(h >= 26 ? 15 : 12);
    return true;
}

SDL_Color StatusBar::stateColor(LinkState s) const
{
    switch (s) {
        case LinkState::Idle:        return SDL_Color{150, 150, 150, 255};
        case LinkState::WaitingPair: return SDL_Color{235, 165, 40, 255};
        case LinkState::Connecting:  return SDL_Color{240, 200, 60, 255};
        case LinkState::Streaming:   return SDL_Color{90, 205, 100, 255};
        case LinkState::Error:       return SDL_Color{225, 70, 60, 255};
    }
    return SDL_Color{150, 150, 150, 255};
}

void StatusBar::draw(SDL_Renderer* r, const SharedStatus& st) const
{
    // 背板 + 顶边
    SDL_SetRenderDrawColor(r, 0x22, 0x22, 0x26, 255);
    SDL_Rect bg{0, y_, w_, h_};
    SDL_RenderFillRect(r, &bg);
    SDL_SetRenderDrawColor(r, 0x3c, 0x3c, 0x42, 255);
    SDL_RenderDrawLine(r, 0, y_, w_, y_);

    const auto  s   = static_cast<LinkState>(st.state.load(std::memory_order_relaxed));
    const SDL_Color sc = stateColor(s);
    const int   midY = y_ + h_ / 2;

    // 状态点（8x8 方块；字体缺失时这就是全部状态显示）
    SDL_SetRenderDrawColor(r, sc.r, sc.g, sc.b, 255);
    SDL_Rect dot{10, midY - 4, 8, 8};
    SDL_RenderFillRect(r, &dot);

    // 左侧：状态文本（Error 时带原因，过长就截断）
    std::string left = toString(s);
    if (s == LinkState::Error) {
        std::string msg = st.error();
        if (!msg.empty()) {
            if (msg.size() > 70) msg = msg.substr(0, 67) + "...";
            left += ": " + msg;
        }
    }
    const SDL_Color fg{230, 230, 230, 255};
    const int textY = midY - text_.lineHeight() / 2;
    text_.draw(r, 24, textY, left, fg);

    // 右侧：分辨率 | fps | 帧数
    char buf[128];
    const int vw = st.videoW.load(std::memory_order_relaxed);
    const int vh = st.videoH.load(std::memory_order_relaxed);
    const int fps = st.fps.load(std::memory_order_relaxed);
    const uint64_t frames = st.frames.load(std::memory_order_relaxed);
    // 音频只在真有流时才出现（没音频流时显示 0 只会让人以为坏了）
    const uint64_t aframes = st.audioFrames.load(std::memory_order_relaxed);
    const uint64_t aunder  = st.audioUnderruns.load(std::memory_order_relaxed);
    char abuf[48] = "";
    if (aframes > 0)
        std::snprintf(abuf, sizeof abuf, " | a:%llu%s", static_cast<unsigned long long>(aframes),
                      aunder > 0 ? "!" : "");   // "!" = 音频缓冲欠载过
    if (vw > 0 && vh > 0) {
        std::snprintf(buf, sizeof buf, "%dx%d | %d fps | %llu frames%s",
                      vw, vh, fps, static_cast<unsigned long long>(frames), abuf);
    } else {
        std::snprintf(buf, sizeof buf, "allplay (M0: 显示管线未接入)");
    }
    const std::string right = buf;
    const int rightW = text_.measure(right);
    text_.draw(r, w_ - rightW - 10, textY, right, fg);
}

}  // namespace allplay
