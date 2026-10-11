/**
 * @file   media.h
 * @brief  视频输出抽象（解码+上屏一体）
 * @author blt
 */

// ============================================================================
// core/port/media.h —— 视频输出抽象（解码+上屏一体）
// ============================================================================
// core 只把"解好码的 AnnexB 帧"喂给这个接口，具体怎么解码/渲染由平台决定：
//   Android = MediaCodec → SurfaceView（硬件解码，csd 用 avcC/hvcC）
//   桌面    = ffmpeg 解码 → SDL 纹理（仅开发验收用）
// 音频（IAudioOutput）属 M4+，本轮只有空桩。
#pragma once

#include <cstdint>
#include <vector>

namespace core {

enum class VideoCodec { H264, HEVC };

class IVideoOutput {
public:
    virtual ~IVideoOutput() = default;

    /// 收到配置帧（avcC/hvcC）后调用；codecData = ISO BMFF avcC/hvcC 记录（不含 fourcc）。
    virtual void configure(VideoCodec codec, int width, int height,
                           const std::vector<uint8_t>& codecData) = 0;

    /// 喂一帧 AnnexB（00 00 00 01 前缀的 NALU，可能含多 NALU）。
    /// 返回 false 表示未能入队（解码器忙/无输入缓冲，帧被丢弃），
    /// 调用方应重新放闸并等下一个关键帧，避免 P 帧参考链断裂后的花屏累积。
    virtual bool feedAnnexB(const uint8_t* data, size_t len) = 0;

    /// 关键帧恢复：重置解码器 / 参考链（丢关键帧后由核心调用）。
    virtual void reset() = 0;

    /// 新 IDR 前 flush 解码器（清参考链，避免场景切换首帧乱码）。
    virtual void flush() = 0;
};

// ---------------------------------------------------------------------------
// 音频输出（CarPlay 音频流 100/101/102，RTP over UDP）
// ---------------------------------------------------------------------------
enum class AudioCodec { AAC_LC, OPUS, LPCM };

class IAudioOutput {
public:
    virtual ~IAudioOutput() = default;

    /// 协商好音频格式后调用一次（每次 SETUP 音频流重建）。
    virtual void configure(AudioCodec codec, int sampleRate, int channels) = 0;

    /// 喂一个解密后的 RTP 载荷（一帧音频数据）。sample = RTP 时间戳（供重排/时钟）。
    virtual void feed(const uint8_t* payload, size_t len, uint32_t sample) = 0;

    /// 流结束/重建时重置。
    virtual void reset() = 0;
};

}  // namespace core
