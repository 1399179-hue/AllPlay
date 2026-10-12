#include "media/ffmpeg_decoder.h"

#include "common/log.h"
#include "media/ffmpeg_compat.h"

extern "C" {
#include <libavutil/mem.h>
}

#include <cstring>

namespace allplay {

namespace {

constexpr const char* kTag = "解码";

/// 往 out 追加一个 AnnexB NALU（4 字节起始码 + 数据）
void appendNalu(std::vector<uint8_t>* out, const uint8_t* d, size_t n)
{
    static const uint8_t kStart[] = {0x00, 0x00, 0x00, 0x01};
    out->insert(out->end(), kStart, kStart + 4);
    out->insert(out->end(), d, d + n);
}

/// avcC 记录体 → AnnexB extradata（SPS/PPS 各自加起始码）。
/// 布局：[0]=1, [1..3]=profile/compat/level, [4]=长度前缀位数-1, [5]=低 5 位 SPS 个数,
///       然后 × N { u16BE len + SPS }，再 1 字节 PPS 个数，然后 × M { u16BE len + PPS }。
/// 参照安卓 MediaCodecSink.buildCsd() 的 H.264 分支。
bool avcCToAnnexB(const std::vector<uint8_t>& c, std::vector<uint8_t>* out)
{
    if (c.size() < 7) return false;
    size_t off = 5;
    const int numSps = c[off++] & 0x1f;
    for (int i = 0; i < numSps; ++i) {
        if (off + 2 > c.size()) return false;
        const size_t len = (size_t(c[off]) << 8) | size_t(c[off + 1]);
        off += 2;
        if (off + len > c.size()) return false;
        appendNalu(out, c.data() + off, len);
        off += len;
    }
    if (off >= c.size()) return false;
    const int numPps = c[off++];
    for (int i = 0; i < numPps; ++i) {
        if (off + 2 > c.size()) return false;
        const size_t len = (size_t(c[off]) << 8) | size_t(c[off + 1]);
        off += 2;
        if (off + len > c.size()) return false;
        appendNalu(out, c.data() + off, len);
        off += len;
    }
    return !out->empty();
}

/// hvcC 记录体 → AnnexB extradata（VPS/SPS/PPS 逐数组取出，各自加起始码）。
/// 布局：… [21] 低 2 位 = 长度前缀位数-1，[22] = 数组个数，
///       然后 × N { 1B: 类型(低 6 位) + u16BE 个数 + × M { u16BE len + NAL } }。
bool hvcCToAnnexB(const std::vector<uint8_t>& c, std::vector<uint8_t>* out)
{
    if (c.size() < 23) return false;
    size_t off = 22;
    const int numArrays = c[off++];
    for (int a = 0; a < numArrays; ++a) {
        if (off + 3 > c.size()) return false;
        off += 1;   // 数组头（complete/类型），类型不关心：VPS/SPS/PPS 全带上
        const int numNalus = (int(c[off]) << 8) | int(c[off + 1]);
        off += 2;
        for (int i = 0; i < numNalus; ++i) {
            if (off + 2 > c.size()) return false;
            const size_t len = (size_t(c[off]) << 8) | size_t(c[off + 1]);
            off += 2;
            if (off + len > c.size()) return false;
            appendNalu(out, c.data() + off, len);
            off += len;
        }
    }
    return !out->empty();
}

}  // namespace

FfmpegVideoDecoder::~FfmpegVideoDecoder() { close(); }

void FfmpegVideoDecoder::close()
{
    if (ctx_) avcodec_free_context(&ctx_);
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    ctx_ = nullptr;
    pkt_ = nullptr;
    frame_ = nullptr;
}

FfmpegVideoDecoder::Action
FfmpegVideoDecoder::configure(core::VideoCodec codec, const std::vector<uint8_t>& codecData)
{
    // 手机回应 forceKeyFrame 时会重发同一份 avcC —— 配置没变就别重建（否则周期性黑帧）
    if (ctx_ && codec == codec_ && codecData == codecData_) return Action::Unchanged;

    // 先把 extradata 备好：失败就别拆掉现有解码器（能凑合就凑合）
    std::vector<uint8_t> extradata;
    const bool parsed = (codec == core::VideoCodec::HEVC) ? hvcCToAnnexB(codecData, &extradata)
                                                          : avcCToAnnexB(codecData, &extradata);
    if (!parsed) {
        LOG_ERROR("%s：codecData 解析失败（%s，%zu 字节），拿不到参数集",
                  kTag, codec == core::VideoCodec::HEVC ? "hvcC" : "avcC", codecData.size());
        return Action::Failed;
    }

    const AVCodec* dec = avcodec_find_decoder(codec == core::VideoCodec::HEVC ? AV_CODEC_ID_HEVC
                                                                             : AV_CODEC_ID_H264);
    if (!dec) {
        LOG_ERROR("%s：本机 ffmpeg 没编入 %s 解码器",
                  kTag, codec == core::VideoCodec::HEVC ? "HEVC" : "H.264");
        return Action::Failed;
    }

    close();
    ctx_ = avcodec_alloc_context3(dec);
    if (!ctx_) return Action::Failed;

    // extradata 用 AnnexB（带起始码）—— 若首字节是 0x01，解码器会以为数据是
    // 长度前缀格式，之后喂 AnnexB 全解错。必须给 +padding 的零填充缓冲。
    ctx_->extradata = static_cast<uint8_t*>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!ctx_->extradata) {
        close();
        return Action::Failed;
    }
    std::memcpy(ctx_->extradata, extradata.data(), extradata.size());
    ctx_->extradata_size = int(extradata.size());

    // 只开 slice 线程：帧线程（frame threading）会把输出整体延后 N 帧（N≈核数），
    // 触摸回传对延迟很敏感，不划算。单 slice 的流会自然退化成单线程。
    ctx_->thread_count = 0;
    ctx_->thread_type  = FF_THREAD_SLICE;

    const int ret = avcodec_open2(ctx_, dec, nullptr);
    if (ret < 0) {
        LOG_ERROR("%s：avcodec_open2 失败（%d）", kTag, ret);
        close();
        return Action::Failed;
    }

    pkt_   = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!pkt_ || !frame_) {
        close();
        return Action::Failed;
    }

    codec_     = codec;
    codecData_ = codecData;
    // 协商值只是参考；真实尺寸等第一帧解出来再更新
    width_ = 0;
    height_ = 0;

    LOG_INFO("%s：解码器已配置 %s，extradata %zu 字节（参数集已转 AnnexB）",
             kTag, codec == core::VideoCodec::HEVC ? "HEVC" : "H.264", extradata.size());
    return Action::Rebuilt;
}

void FfmpegVideoDecoder::receiveAll(const std::function<void(const AVFrame*)>& onFrame)
{
    while (true) {
        const int ret = avcodec_receive_frame(ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
        if (ret < 0) {
            LOG_WARN("%s：avcodec_receive_frame 出错（%d）", kTag, ret);
            return;
        }
        if (frame_->width > 0 && frame_->height > 0) {
            width_  = frame_->width;
            height_ = frame_->height;
        }
        onFrame(frame_);
        av_frame_unref(frame_);
    }
}

bool FfmpegVideoDecoder::decode(const uint8_t* data, size_t len,
                                const std::function<void(const AVFrame*)>& onFrame)
{
    if (!ctx_ || !pkt_) return false;

    // 先把上一轮攒下的输出取干净，避免下面 send 撞 EAGAIN
    receiveAll(onFrame);

    av_packet_unref(pkt_);
    if (av_new_packet(pkt_, int(len)) < 0) return false;
    std::memcpy(pkt_->data, data, len);

    const int ret = avcodec_send_packet(ctx_, pkt_);
    if (ret < 0) {
        // EAGAIN：drain 过一轮还塞不进去，说明解码器积压 —— 丢这一帧但不当作致命错误
        if (ret == AVERROR(EAGAIN)) {
            LOG_WARN("%s：解码器积压，丢弃一帧（%zu 字节）", kTag, len);
            return true;
        }
        LOG_WARN("%s：avcodec_send_packet 出错（%d）", kTag, ret);
        return false;
    }

    receiveAll(onFrame);
    return true;
}

void FfmpegVideoDecoder::reset()
{
    if (ctx_) avcodec_flush_buffers(ctx_);
}

}  // namespace allplay
