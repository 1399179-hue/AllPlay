/**
 * @file   ffmpeg_compat.h
 * @brief  ffmpeg 版本差异的单点封装
 * @author blt
 */

// ============================================================================
// host/media/ffmpeg_compat.h —— ffmpeg 版本差异的单点封装
// ============================================================================
// 开发机（macOS/brew）是 ffmpeg 8.x（libavcodec 63），目标机（Ubuntu 22.04）
// 是 ffmpeg 4.4（libavcodec 58）—— 两边**没有共同的新式声道 API**：
//   · `AVChannelLayout` / `ch_layout` 是 5.1（libavcodec 59.24.100）才引入的；
//   · 老的 `channels` / `channel_layout` 字段在 7.0 又被删掉了。
// 所以本目录一律遵守三条：
//   1. 版本分支**只写在这里**，别处只用下面的包装；
//   2. 解码器完全不设声道布局（让解码器从码流自己推），只**读**帧里的声道数；
//   3. 重采样自己写（见 sdl_audio_sink.cpp），不引 swresample —— 顺带绕开它整套
//      channel-layout 参数在新旧版之间的签名差异。
#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/version.h>
}

/// ch_layout 取代 channels 的分界 = libavcodec 59.24.100（ffmpeg 5.1）
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
#  define ALLPLAY_AV_NEW_CHLAYOUT 1
#else
#  define ALLPLAY_AV_NEW_CHLAYOUT 0
#endif

namespace allplay {

/// 解码帧的声道数（新旧两套 API 的取值点）
inline int frameChannels(const AVFrame* f)
{
#if ALLPLAY_AV_NEW_CHLAYOUT
    return f->ch_layout.nb_channels;
#else
    return f->channels;
#endif
}

}  // namespace allplay
