/**
 * @file   annexb.h
 * @brief  裸 H.264 基本流的解析与 avcC/AVCC 互转（video_probe 与 fake_phone 共用）
 * @author blt
 */

// ============================================================================
// tools/annexb.h —— AnnexB 解析 + avcC 合成 + AnnexB→AVCC
// ============================================================================
// **为什么让两个工具共用同一份**：video_probe 的全部价值在于"二分"——
//   出画面 → 问题在协议侧（RTSP / 加密 / 分帧 / 配置帧顺序）
//   不出画面 → 问题在解码侧（extradata / 像素格式 / 纹理）
// 这条二分线成立的前提是两边**对同一段码流做出完全相同的解释**：同样的 AU 切法、
// 同样的 avcC。各自抄一份迟早会分叉，那时"probe 有画面、真机黑屏"就说明不了任何事。
//
// 只做解析，不碰 ffmpeg —— 所以 fake_phone 不需要链任何解码库。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace allplay::annexb {

struct NalRef {
    size_t scStart;   ///< 起始码首字节下标
    size_t payload;   ///< NAL 首字节（已跳过起始码）
    size_t end;       ///< NAL 末尾（已剥掉尾部 0x00）
    int    type;      ///< nal_unit_type（H.264 低 5 位）
};

inline bool isVcl(int t) { return t >= 1 && t <= 5; }

/// 扫出全部 NAL。3/4 字节起始码都能认：4 字节码的前导 0 会被算作上一段的
/// 尾部 0x00，正好被剥掉（规范里 NAL 尾部的 0x00 本来就是 trailing_zero_8bits）。
inline std::vector<NalRef> scanNals(const std::vector<uint8_t>& b)
{
    std::vector<NalRef> out;
    const size_t n = b.size();

    auto findSc = [&](size_t from, size_t* sc, size_t* payload) -> bool {
        for (size_t k = from; k + 3 <= n; ++k) {
            if (b[k] == 0 && b[k + 1] == 0 && b[k + 2] == 1) {
                *sc = k;
                *payload = k + 3;
                return true;
            }
        }
        return false;
    };

    size_t sc = 0, pl = 0;
    if (!findSc(0, &sc, &pl)) return out;
    for (;;) {
        size_t nsc = 0, npl = 0;
        const bool more = findSc(pl, &nsc, &npl);
        size_t end = more ? nsc : n;
        while (end > pl && b[end - 1] == 0) --end;
        if (end > pl) out.push_back(NalRef{sc, pl, end, int(b[pl] & 0x1f)});
        if (!more) break;
        sc = nsc;
        pl = npl;
    }
    return out;
}

/// 切访问单元：VCL 之后再来一个 NAL，都算新 AU 开始。
/// 返回每个 AU 在原缓冲里的 [begin, end) —— 含起始码。
inline std::vector<std::pair<size_t, size_t>> splitAus(const std::vector<uint8_t>& b,
                                                       const std::vector<NalRef>& nals)
{
    std::vector<std::pair<size_t, size_t>> aus;
    if (nals.empty()) return aus;

    size_t auStart = nals[0].scStart;
    bool   auHasVcl = isVcl(nals[0].type);
    for (size_t i = 1; i < nals.size(); ++i) {
        const bool vcl = isVcl(nals[i].type);
        if (auHasVcl) {   // 本 AU 已有 VCL 片，后面任何 NAL 都归属下一个 AU
            aus.push_back({auStart, nals[i].scStart});
            auStart = nals[i].scStart;
        }
        auHasVcl = vcl;
    }
    aus.push_back({auStart, b.size()});
    return aus;
}

/// 用文件里的 SPS/PPS 合成 avcC 记录体（首字节 0x01）。
/// **故意绕这么一圈**：core 交给解码器的就是 avcC，本地冒烟必须走同一条路，
/// 否则测不出 extradata 那个坑（见 host/media/ffmpeg_decoder.h 的注释）。
inline bool buildAvcC(const std::vector<uint8_t>& b, const std::vector<NalRef>& nals,
                      std::vector<uint8_t>* out, std::string* err)
{
    const NalRef* sps = nullptr;
    std::vector<const NalRef*> ppss;
    for (const auto& r : nals) {
        if (r.type == 7 && !sps) sps = &r;
        else if (r.type == 8 && ppss.size() < 255) ppss.push_back(&r);
    }
    if (!sps || ppss.empty()) {
        *err = "文件里没有 SPS/PPS —— 大概不是 AnnexB 裸流；"
               "用 ffmpeg -i in.mp4 -c:v libx264 -bsf:v h264_mp4toannexb -an out.h264 重转一次";
        return false;
    }
    if (sps->end - sps->payload < 4) {
        *err = "SPS 短得不像话";
        return false;
    }

    auto put16 = [&](size_t v) {
        out->push_back(uint8_t(v >> 8));
        out->push_back(uint8_t(v));
    };

    out->clear();
    out->push_back(1);                              // configurationVersion
    out->push_back(b[sps->payload + 1]);            // profile_idc
    out->push_back(b[sps->payload + 2]);            // constraint flags
    out->push_back(b[sps->payload + 3]);            // level_idc
    out->push_back(0xff);                           // 长度前缀恒为 4 字节
    out->push_back(0xe0 | uint8_t(1));              // 保留位 | SPS 个数 = 1
    put16(sps->end - sps->payload);
    out->insert(out->end(), b.begin() + sps->payload, b.begin() + sps->end);
    out->push_back(uint8_t(ppss.size()));
    for (const NalRef* p : ppss) {
        put16(p->end - p->payload);
        out->insert(out->end(), b.begin() + p->payload, b.begin() + p->end);
    }
    return true;
}

/// 一个 AnnexB 访问单元 → AVCC（每个 NALU 前加 4 字节大端长度）。
/// 这是**推流方向**的转换：CarPlay 屏幕流里跑的就是 AVCC（因为配置帧给的是 avcC），
/// core 收到后会自己 avccToAnnexB（core/airplay/video_stream.cpp:170）。
/// 返回空 = 这段不是合法的 AnnexB。
inline std::vector<uint8_t> toAvcc(const std::vector<uint8_t>& b)
{
    const auto nals = scanNals(b);
    if (nals.empty()) return {};

    std::vector<uint8_t> out;
    out.reserve(b.size() + 4 * nals.size());
    for (const auto& r : nals) {
        const size_t len = r.end - r.payload;
        if (len == 0 || len > 0xFFFFFF) return {};
        out.push_back(uint8_t(len >> 24));
        out.push_back(uint8_t(len >> 16));
        out.push_back(uint8_t(len >> 8));
        out.push_back(uint8_t(len));
        out.insert(out.end(), b.begin() + r.payload, b.begin() + r.end);
    }
    return out;
}

}  // namespace allplay::annexb
