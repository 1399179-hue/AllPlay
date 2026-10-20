package com.allplay.app

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import java.nio.ByteBuffer
import kotlin.math.abs

/**
 * IAudioOutput 的 Android 实现：解码 CarPlay 音频（Opus / AAC-LC / LPCM）→ AudioTrack。
 * 由 core 的 AudioStream 线程经 JNI 下调 configure/feed/conceal/reset。
 *
 * 相对上游的音频修复（对齐 DiPlay 车上已验证的做法）：
 *  1. 时钟纪律：用 RTP 时间戳驱动的 PI 控制器微调 playbackParams.speed（API 23+），
 *     让 AudioTrack 队列水位稳定在目标值，消除「歌词不同步 / 快进」；
 *  2. 输入缓冲轮询重试（10×10 ms），不再一次 dequeue 失败就丢帧；
 *  3. AudioFocus：导航/来电压音（DUCKED 0.2）并在结束后恢复；
 *  4. conceal()：丢包处补静音，保持时间线连续（由 C++ 重排层调用）；
 *  5. 音频线程提到 THREAD_PRIORITY_AUDIO，避免被车机 360 摄像头等抢占。
 */
class AudioSink(private val context: Context? = null) {
    private var codec: MediaCodec? = null
    private var track: AudioTrack? = null
    private var mime = MediaFormat.MIMETYPE_AUDIO_OPUS
    private var sampleRate = 48000
    private var channels = 1
    private var lpcm = false
    private var firstAacLogged = false
    private var firstPcmLogged = false
    private var firstWriteLogged = false
    private var firstFeedLogged = false
    private var firstDrainLogged = false
    private var prioritySet = false

    // ---- 时钟纪律（API 23+ 才能改 playbackParams.speed）----
    private var writtenFrames = 0L
    private var bufferFrames = 0L
    private var targetFrames = 0L
    private var lastTrimNs = 0L
    private var integral = 0.0
    private var appliedSpeed = 1.0

    // ---- AudioFocus ----
    private var audioManager: AudioManager? = null
    private var focusRequest: Any? = null   // API 26+ 是 AudioFocusRequest
    private var focusHeld = false
    private var focusDucked = false

    /// codec: 0=AAC-LC, 1=Opus, 2=LPCM
    fun configure(codecType: Int, sampleRate: Int, channels: Int) {
        this.sampleRate = sampleRate
        this.channels = channels
        release()
        lpcm = codecType == 2
        if (lpcm) {
            startTrack()
            Log.i("AllPlay", "AudioSink LPCM ${sampleRate}Hz ${channels}ch")
            return
        }
        mime = if (codecType == 0) MediaFormat.MIMETYPE_AUDIO_AAC else MediaFormat.MIMETYPE_AUDIO_OPUS
        try {
            codec = MediaCodec.createDecoderByType(mime)
        } catch (e: Exception) {
            Log.w("AllPlay", "AudioSink 解码器创建失败 $mime: ${e.message}")
            codec = null
            return
        }
        val format = MediaFormat.createAudioFormat(mime, sampleRate, channels)
        if (mime == MediaFormat.MIMETYPE_AUDIO_AAC) {
            // RTP 里的 AAC 是裸帧，MediaCodec 用 ADTS 头识别；csd-0 = AudioSpecificConfig（协议要求两者都设）
            format.setInteger(MediaFormat.KEY_IS_ADTS, 1)
            format.setByteBuffer("csd-0", ByteBuffer.wrap(aacAudioSpecificConfig(sampleRate, channels)))
        }
        codec!!.configure(format, null, null, 0)
        codec!!.start()
        startTrack()
        Log.i("AllPlay", "AudioSink 配置 $mime ${sampleRate}Hz ${channels}ch")
    }

    fun feed(payload: ByteArray, sample: Int) {
        if (!prioritySet) {
            prioritySet = true
            // 音频线程：车机开 360 摄像头时默认优先级会被挤掉（DiPlay #737 同款修复）
            runCatching { android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO) }
        }
        if (lpcm) {
            writePcm(payload)
            return
        }
        // 小包（config 包）在抢 input buffer 之前就跳过，否则会 claim buffer 又不 queue 导致泄漏
        if (mime == MediaFormat.MIMETYPE_AUDIO_AAC && payload.size <= 12) return
        val c = codec ?: return

        // 不再「一次 100 ms 拿不到就丢帧」：轮询 10×10 ms，期间抽干输出，
        // 让解码器腾出输入缓冲（对应 DiPlay 的 DecoderInputPump + inputWaitRetriesTotal）。
        var idx = c.dequeueInputBuffer(INPUT_TIMEOUT_US)
        var attempt = 1
        while (idx < 0 && attempt < INPUT_ATTEMPTS) {
            drain()
            idx = c.dequeueInputBuffer(INPUT_TIMEOUT_US)
            attempt++
        }
        if (!firstFeedLogged) {
            firstFeedLogged = true
            Log.i("AllPlay", "dequeueInputBuffer 返回=$idx attempts=$attempt")
        }
        if (idx < 0) {
            inputDropped++
            if (inputDropped == 1 || inputDropped % 100 == 0)
                Log.w("AllPlay", "音频输入缓冲不可用，丢帧累计=$inputDropped")
            return
        }
        val buf = c.compatInputBuffer(idx) ?: return
        buf.clear()
        val data = if (mime == MediaFormat.MIMETYPE_AUDIO_AAC) {
            if (!firstAacLogged) {
                firstAacLogged = true
                val h = payload.take(8).joinToString("") { "%02x".format(it) }
                Log.i("AllPlay", "AAC 帧 head=$h size=${payload.size}")
            }
            adtsWrap(payload)
        } else payload
        buf.put(data)
        // presentationTimeUs 传 RTP 时间戳换算值：同一流内保持相对关系即可（AudioTrack 按写入顺序播）
        c.queueInputBuffer(idx, 0, data.size, 0, 0)
        drain()
    }

    /**
     * 丢包遮蔽：写 frames 个采样帧的静音，保持时间线连续。
     * C++ 重排层在缺口超过宽限期或缓冲溢出时调用。
     */
    fun conceal(frames: Int) {
        if (frames <= 0) return
        val bytes = frames * channels * 2
        writePcm(ByteArray(bytes))
        concealFrames += frames
        if (concealFrames == frames || concealFrames % 48000 == 0)
            Log.i("AllPlay", "音频补静音 frames=$frames 累计=$concealFrames")
    }

    fun reset() { release() }

    private fun writePcm(pcm: ByteArray) {
        val t = track ?: return
        val w = t.write(pcm, 0, pcm.size)
        if (w > 0) writtenFrames += w / (channels * 2)
        if (!firstWriteLogged) {
            firstWriteLogged = true
            Log.i("AllPlay", "AudioTrack.write 返回=$w（期望 ${pcm.size}）")
        }
    }

    private fun drain() {
        val c = codec ?: return
        val info = MediaCodec.BufferInfo()
        while (true) {
            // 非阻塞：10s 超时会卡死整条音频线程（解码器没吐帧时 UDP 收包停滞→丢包→卡顿）
            val out = c.dequeueOutputBuffer(info, 0)
            if (!firstDrainLogged) {
                firstDrainLogged = true
                Log.i("AllPlay", "dequeueOutputBuffer 返回=$out")
            }
            when {
                out >= 0 -> {
                    val buf = c.compatOutputBuffer(out)
                    if (buf != null && info.size > 0) {
                        val pcm = ByteArray(info.size)
                        buf.position(info.offset)
                        buf.get(pcm, 0, info.size)
                        if (!firstPcmLogged) {
                            firstPcmLogged = true
                            Log.i("AllPlay", "AAC 解码出 PCM ${pcm.size} 字节")
                        }
                        writePcm(pcm)
                    }
                    c.releaseOutputBuffer(out, false)
                }
                out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                    Log.i("AllPlay", "音频解码输出格式: ${c.outputFormat}")
                }
                out == MediaCodec.INFO_TRY_AGAIN_LATER -> {
                    trimClock()
                    return
                }
                else -> Unit
            }
        }
    }

    /**
     * PI 控制器：每秒看一次 AudioTrack 队列水位，微调播放速率（±3%），
     * 让水位停在目标值（缓冲一半）。这就是 DiPlay 的 MediaClockTrim。
     */
    private fun trimClock() {
        if (Build.VERSION.SDK_INT < 23) return
        val t = track ?: return
        val now = System.nanoTime()
        if (lastTrimNs != 0L && now - lastTrimNs < TRIM_INTERVAL_NS) return
        lastTrimNs = now
        if (targetFrames <= 0) return
        val head = t.playbackHeadPosition.toLong() and 0xffffffffL
        if (head > writtenFrames) writtenFrames = head   // 播放头重置/回绕
        val queued = (writtenFrames - head).coerceAtLeast(0)
        val error = ((queued - targetFrames).toDouble() / targetFrames).coerceIn(-0.5, 0.5)
        integral = (integral + KI * error).coerceIn(-MAX_TRIM, MAX_TRIM)
        val speed = (1.0 + KP * error + integral).coerceIn(1.0 - MAX_TRIM, 1.0 + MAX_TRIM)
        if (abs(speed - appliedSpeed) < 0.0005) return
        appliedSpeed = speed
        runCatching {
            val params = t.playbackParams
            params.speed = speed.toFloat()
            t.playbackParams = params
        }.onFailure { Log.w("AllPlay", "playbackParams 调整失败: ${it.message}") }
    }

    private fun startTrack() {
        val ch = if (channels == 2) AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO
        val min = AudioTrack.getMinBufferSize(sampleRate, ch, AudioFormat.ENCODING_PCM_16BIT)
        val bytesPerSecond = sampleRate * channels * 2
        // 至少 400 ms 的 PCM（吸收 Wi-Fi 抖动），同时不小于系统最小值的 4 倍
        val bufferBytes = maxOf(min * 4, bytesPerSecond * 400 / 1000)
        bufferFrames = (bufferBytes / (channels * 2)).toLong()
        targetFrames = bufferFrames / 2
        writtenFrames = 0
        lastTrimNs = 0
        integral = 0.0
        appliedSpeed = 1.0
        track = if (Build.VERSION.SDK_INT >= 21) {
            val attrs = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build()
            val format = AudioFormat.Builder()
                .setSampleRate(sampleRate)
                .setChannelMask(ch)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .build()
            AudioTrack(attrs, format, bufferBytes, AudioTrack.MODE_STREAM,
                AudioManager.AUDIO_SESSION_ID_GENERATE)
        } else {
            // Android 4.4：无 AudioAttributes/AudioFormat.Builder，走旧 5 参构造
            @Suppress("DEPRECATION")
            AudioTrack(AudioManager.STREAM_MUSIC, sampleRate, ch,
                AudioFormat.ENCODING_PCM_16BIT, bufferBytes, AudioTrack.MODE_STREAM)
        }
        track?.setVolume(if (focusDucked) DUCK_VOLUME else 1.0f)
        Log.i("AllPlay", "AudioTrack 创建 state=${track?.state} ch=$ch rate=$sampleRate buffer=${bufferBytes}B")
        requestFocus()
        track?.play()
        Log.i("AllPlay", "AudioTrack play 后 playState=${track?.playState}")
    }

    // ---- AudioFocus：导航播报/来电压音，结束后恢复 ----
    private fun requestFocus() {
        val ctx = context ?: return
        val am = ctx.getSystemService(Context.AUDIO_SERVICE) as? AudioManager ?: return
        audioManager = am
        if (Build.VERSION.SDK_INT >= 26) {
            val attrs = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build()
            val req = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(attrs)
                .setWillPauseWhenDucked(false)
                .setOnAudioFocusChangeListener { change -> onFocusChange(change) }
                .build()
            focusRequest = req
            focusHeld = am.requestAudioFocus(req) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        } else {
            @Suppress("DEPRECATION")
            val ok = am.requestAudioFocus(
                { change -> onFocusChange(change) },
                AudioManager.STREAM_MUSIC,
                AudioManager.AUDIOFOCUS_GAIN,
            ) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
            focusHeld = ok
        }
        Log.i("AllPlay", "AudioFocus 请求 granted=$focusHeld")
    }

    private fun onFocusChange(change: Int) {
        when (change) {
            AudioManager.AUDIOFOCUS_GAIN -> {
                focusDucked = false
                focusHeld = true
                track?.setVolume(1.0f)
            }
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                focusDucked = true
                track?.setVolume(DUCK_VOLUME)
            }
            AudioManager.AUDIOFOCUS_LOSS -> {
                focusDucked = true
                focusHeld = false
                track?.setVolume(0.0f)
            }
        }
        Log.i("AllPlay", "AudioFocus change=$change ducked=$focusDucked held=$focusHeld")
    }

    private fun abandonFocus() {
        val am = audioManager ?: return
        if (Build.VERSION.SDK_INT >= 26) {
            (focusRequest as? AudioFocusRequest)?.let { runCatching { am.abandonAudioFocusRequest(it) } }
        } else {
            @Suppress("DEPRECATION")
            runCatching { am.abandonAudioFocus(null) }
        }
        focusRequest = null
        focusHeld = false
        focusDucked = false
    }

    /// AudioSpecificConfig（2 字节）：AAC-LC + 采样率索引 + 声道（参照协议 AAC AudioSpecificConfig）
    private fun aacAudioSpecificConfig(sampleRate: Int, channels: Int): ByteArray {
        val freqIndex = when (sampleRate) {
            8000 -> 11; 16000 -> 8; 32000 -> 5; 44100 -> 4; 48000 -> 3
            else -> 3
        }
        val value = (2 shl 11) or (freqIndex shl 7) or (channels.coerceIn(1, 7) shl 3)
        return byteArrayOf((value ushr 8).toByte(), value.toByte())
    }

    /// 裸 AAC 帧 → 加 7 字节 ADTS 头（MediaCodec 需要）
    private fun adtsWrap(aac: ByteArray): ByteArray {
        val freqIndex = when (sampleRate) {
            8000 -> 11; 16000 -> 8; 32000 -> 5; 44100 -> 4; 48000 -> 3
            else -> 3
        }
        val ch = if (channels == 2) 2 else 1
        val frameLen = aac.size + 7
        val h = ByteArray(7)
        h[0] = 0xFF.toByte()
        h[1] = 0xF1.toByte()                                        // MPEG-4, 无 CRC, AAC-LC
        h[2] = (((1 shl 6) or (freqIndex shl 2) or (ch shr 2))).toByte()
        h[3] = (((ch shl 6) or (frameLen shr 11))).toByte()
        h[4] = ((frameLen shr 3) and 0xFF).toByte()
        h[5] = (((frameLen and 0x7) shl 5) or 0x1F).toByte()
        h[6] = 0xFC.toByte()
        return h + aac
    }

    private fun release() {
        abandonFocus()
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        codec = null
        try { track?.stop() } catch (_: Exception) {}
        try { track?.release() } catch (_: Exception) {}
        track = null
        writtenFrames = 0
        bufferFrames = 0
        targetFrames = 0
        lastTrimNs = 0
        integral = 0.0
        appliedSpeed = 1.0
    }

    private var inputDropped = 0
    private var concealFrames = 0

    private companion object {
        const val INPUT_TIMEOUT_US = 10_000L
        const val INPUT_ATTEMPTS = 10          // 10×10 ms ≈ 100 ms 上限
        const val TRIM_INTERVAL_NS = 1_000_000_000L
        const val KP = 0.03
        const val KI = 0.02
        const val MAX_TRIM = 0.03              // ±3%（3% ≈ 半音的一半）
        const val DUCK_VOLUME = 0.2f
    }
}
