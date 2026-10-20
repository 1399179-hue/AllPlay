#include "airplay/audio_stream.h"

#include "common/log.h"
#include "crypto/chacha20poly1305.h"
#include "crypto/hkdf.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace core::airplay {

namespace {
constexpr size_t kRtpHeader  = 12;
constexpr size_t kNonceTail  = 8;
constexpr size_t kMaxDatagram = 4096;
// 短超时：缺口要能被及时补静音，而不是等 1 s 才发现。
constexpr int    kTimeoutMs  = 20;
// 重排缓冲上限：12 包 ≈ 256 ms（AAC 48 kHz 每包 21.3 ms）。
constexpr size_t kMaxJitter  = 12;
// 缺口宽限期：给乱序/网络抖动留 60 ms，超过就补静音继续。
constexpr uint64_t kGapGraceNs = 60ull * 1000 * 1000;
// 一次最多补 8 包（≈170 ms），避免长时间中断后补出几秒静音。
constexpr uint64_t kMaxConcealPackets = 8;

uint64_t nowNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
}  // namespace

AudioStream::AudioStream(std::function<std::unique_ptr<core::IUdpSocket>()> makeUdp,
                         const std::vector<uint8_t>& sharedSecret,
                         const std::string& streamConnectionIdDecimal,
                         AudioCodec codec, int sampleRate, int channels,
                         core::IAudioOutput* output)
    : codec_(codec), sampleRate_(sampleRate), channels_(channels), output_(output)
{
    const auto key = crypto::hkdfSha512(sharedSecret,
                                        "DataStream-Salt" + streamConnectionIdDecimal,
                                        "DataStream-Output-Encryption-Key", 32);
    std::copy(key.begin(), key.end(), key_.begin());

    std::string err;
    dataSock_ = makeUdp();
    if (!dataSock_ || !dataSock_->bind(0, &err)) {
        LOG_WARN("音频数据 UDP 绑定失败：%s", err.c_str());
        return;
    }
    dataPort_ = dataSock_->port();

    controlSock_ = makeUdp();
    if (controlSock_) controlSock_->bind(0, &err);
    controlPort_ = controlSock_ ? controlSock_->port() : 0;

    stop_.store(false);
    thread_ = std::thread([this] { run(); });
    LOG_INFO("音频流就绪 dataPort=%u controlPort=%u（codec=%d %dHz %dch）",
             dataPort_, controlPort_, int(codec_), sampleRate_, channels_);
}

AudioStream::~AudioStream() { stop(); }

void AudioStream::stop()
{
    if (stop_.exchange(true)) return;
    if (dataSock_) dataSock_->close();
    if (controlSock_) controlSock_->close();
    if (thread_.joinable()) thread_.join();
}

void AudioStream::run()
{
    uint8_t buf[kMaxDatagram];
    while (!stop_.load()) {
        const int n = dataSock_->recv(buf, sizeof buf, kTimeoutMs);
        if (n <= 0) {   // 超时/关闭：仍要检查缺口，否则丢包后会一直等下去
            maybeConceal(nowNs());
            continue;
        }

        // wire = [12B RTP 头][密文][16B tag][8B nonce]
        if (size_t(n) < kRtpHeader + 16 + kNonceTail) continue;

        std::array<uint8_t, 12> nonce{};
        std::memcpy(nonce.data() + 4, buf + n - kNonceTail, kNonceTail);   // 4 零 + 尾部 8B

        const size_t ctLen = size_t(n) - kRtpHeader - kNonceTail;          // 密文 + tag
        std::vector<uint8_t> pt;
        if (!crypto::chachaOpen(key_.data(), nonce,
                                buf + 4, 8,             // AAD = 头[4..12]（timestamp+SSRC）
                                buf + kRtpHeader, ctLen,
                                &pt))
            continue;   // 鉴权失败（丢包，非致命）

        const uint32_t sample = (uint32_t(buf[4]) << 24) | (uint32_t(buf[5]) << 16) |
                                (uint32_t(buf[6]) << 8) | uint32_t(buf[7]);
        const uint16_t seq = uint16_t((buf[2] << 8) | buf[3]);   // RTP 序号（乱序重排用）
        if (pt.size() > 0) {
            static uint64_t audioPkts = 0;
            if (++audioPkts == 1) {
                char hx[64] = "";
                for (size_t k = 0; k < pt.size() && k < 16; ++k)
                    std::snprintf(hx + std::strlen(hx), sizeof hx - std::strlen(hx), "%02x", pt[k]);
                LOG_INFO("音频包 #1 payload=%zuB head=%s", pt.size(), hx);
            } else if (audioPkts % 500 == 0) {
                LOG_INFO("音频包 #%llu payload=%zuB sample=%u", (unsigned long long)audioPkts,
                         pt.size(), sample);
            }
        }
        deliver(pt.data(), pt.size(), sample, seq);
        maybeConceal(nowNs());
    }
}

size_t AudioStream::frameSamples() const
{
    switch (codec_) {
        case AudioCodec::AAC_LC: return 1024;   // AAC 一帧 1024 采样
        case AudioCodec::OPUS:   return 960;    // Opus 20 ms @48 kHz
        default:                 return 0;      // LPCM 无帧概念，不遮蔽
    }
}

void AudioStream::concealPackets(uint64_t packets)
{
    if (!output_ || packets == 0) return;
    const size_t fs = frameSamples();
    if (!fs) return;
    const uint64_t n = std::min<uint64_t>(packets, kMaxConcealPackets);
    for (uint64_t i = 0; i < n; ++i) output_->conceal(fs);
    static uint64_t concealEvents = 0;
    if (++concealEvents % 100 == 1)
        LOG_INFO("音频丢包遮蔽：本次补 %llu 包静音（累计 %llu 次）",
                 (unsigned long long)n, (unsigned long long)concealEvents);
}

void AudioStream::flushContiguous()
{
    while (!pending_.empty() && pending_.begin()->first == nextSeq_) {
        auto it = pending_.begin();
        if (output_) output_->feed(it->second.payload.data(), it->second.payload.size(), it->second.sample);
        pending_.erase(it);
        ++nextSeq_;
    }
    gapSinceNs_ = 0;
}

void AudioStream::maybeConceal(uint64_t now)
{
    if (pending_.empty()) {
        gapSinceNs_ = 0;
        return;
    }
    const uint64_t head = pending_.begin()->first;
    if (head <= nextSeq_) {   // 已经有连续包了，顺出即可
        flushContiguous();
        return;
    }
    if (gapSinceNs_ == 0) {   // 缺口刚开始，先等宽限期（乱序包可能马上到）
        gapSinceNs_ = now;
        return;
    }
    if (now - gapSinceNs_ < kGapGraceNs) return;

    // 宽限期已过：补齐静音，时间线继续，而不是一直卡住或整段跳变
    concealPackets(head - nextSeq_);
    nextSeq_ = head;
    gapSinceNs_ = 0;
    flushContiguous();
}

void AudioStream::deliver(const uint8_t* payload, size_t len, uint32_t sample, uint16_t seq)
{
    // 16-bit RTP 序号 → 64-bit 扩展序号（有符号增量同时处理回绕与乱序）
    if (!haveSeq_) {
        haveSeq_ = true;
        nextSeq_ = seq;
        lastRaw_ = seq;
        lastExt_ = seq;
    } else {
        lastExt_ += int16_t(uint16_t(seq - lastRaw_));
        lastRaw_ = seq;
    }
    if (lastExt_ < nextSeq_) return;   // 太晚到达（已播放过），丢弃

    pending_[lastExt_] = Pkt{std::vector<uint8_t>(payload, payload + len), sample};

    // 先把能按序吐出的都吐掉
    flushContiguous();

    // 防缓冲无限增长：丢最旧的，但用静音补齐被跳过的那段，避免可闻的跳变/爆音
    while (pending_.size() > kMaxJitter) {
        const uint64_t head = pending_.begin()->first;
        concealPackets(head - nextSeq_);
        nextSeq_ = head + 1;
        pending_.erase(pending_.begin());
        flushContiguous();
    }
}

}  // namespace core::airplay
