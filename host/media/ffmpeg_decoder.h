/**
 * @file   ffmpeg_decoder.h
 * @brief  H.264/HEVC 软解（libavcodec），AnnexB 进、AVFrame 出
 * @author blt
 */

// ============================================================================
// host/media/ffmpeg_decoder.h —— H.264/HEVC 软解（libavcodec）
// ============================================================================
// 只做解码，不碰 SDL —— 把 ffmpeg 的 API 面收在一个文件里，SDK 版本变动好定位。
//
// ⚠️ extradata 的坑（不处理就永久花屏/黑屏）：
//   core 给的 codecData 是 **avcC/hvcC 记录体**（首字节 0x01）。而 ffmpeg 的
//   H.264/HEVC 解码器一旦看到 extradata 首字节是 1，就切到"4 字节长度前缀"模式，
//   之后我们再喂 AnnexB（起始码）会整体解错。所以 configure() 会把 avcC/hvcC 里的
//   SPS/PPS 拆出来、各自加上起始码当 extradata —— 跟安卓
//   MediaCodecSink.buildCsd() 是同一个思路（那边拆出来喂给 MediaCodec 的 csd-N）。
#pragma once

#include "port/media.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstdint>
#include <functional>
#include <vector>
#include <cstddef>

namespace allplay {

class FfmpegVideoDecoder {
public:
    enum class Action {
        Rebuilt,     ///< 新建/重建了解码器
        Unchanged,   ///< 配置与当前完全一致，跳过重建（避免周期性黑帧）
        Failed,      ///< 建不起来（编解码器没编进来 / OOM）
    };

    FfmpegVideoDecoder() = default;
    ~FfmpegVideoDecoder();
    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    /// 按 avcC/hvcC 配置（重建）解码器。
    /// 会先跟上次的 (codec, codecData) 比对：一模一样则返回 Unchanged 不动解码器。
    Action configure(core::VideoCodec codec, const std::vector<uint8_t>& codecData);

    bool usable() const { return ctx_ != nullptr; }

    /// 喂一帧 AnnexB；解出的帧（0..n 张）经 onFrame 回调，回调返回后帧即失效。
    /// 返回 false = 解码器不可用（上层应放闸等关键帧，别继续喂）。
    bool decode(const uint8_t* data, size_t len,
                const std::function<void(const AVFrame*)>& onFrame);

    /// 清参考链（关键帧恢复）。extradata 与已解出的帧不受影响。
    void reset();

    int width() const { return width_; }
    int height() const { return height_; }

private:
    void close();
    void receiveAll(const std::function<void(const AVFrame*)>& onFrame);

    AVCodecContext*      ctx_   = nullptr;
    AVPacket*            pkt_   = nullptr;
    AVFrame*             frame_ = nullptr;
    core::VideoCodec     codec_ = core::VideoCodec::H264;
    std::vector<uint8_t> codecData_;   ///< 上次配置（去重用）
    int                  width_ = 0, height_ = 0;
};

}  // namespace allplay
