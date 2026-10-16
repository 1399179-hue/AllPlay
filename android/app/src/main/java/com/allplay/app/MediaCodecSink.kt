package com.allplay.app

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer

/**
 * IVideoOutput 的 Android 实现：MediaCodec 硬件解码 → Surface。
 * 由 core 的屏幕流线程经 JNI 下调 configure/feedAnnexB/reset。
 * 参照协议（AndroidMediaSink 行为）：H264 csd 用 avcC 里拆出的 SPS/PPS，HEVC 用 hvcC。
 */
class MediaCodecSink {
    private var codec: MediaCodec? = null
    private var mime = MediaFormat.MIMETYPE_VIDEO_AVC
    private var width = 1280
    private var height = 720
    private var surface: Surface? = null

    // surface 未就绪时暂存配置，setSurface 后再补配
    private var pendingCodecType = 0
    private var pendingCodecData: ByteArray? = null
    // 最近一次成功配置，用于 Surface 变更时重建解码器
    private var lastCodecType = 0
    private var lastCodecData: ByteArray? = null
    // configure 失败退避：Surface 抖动时别高频重试，等系统稳定
    private var configureFailUntil = 0L

    /// codec: 0=H264, 1=HEVC；codecData = ISO BMFF avcC/hvcC 记录（不含 fourcc）
    fun configure(codecType: Int, width: Int, height: Int, codecData: ByteArray) {
        val now = System.currentTimeMillis()
        // 退避期：只记下最新配置，等退避结束再真正重建
        if (now < configureFailUntil) {
            this.width = width; this.height = height
            lastCodecType = codecType; lastCodecData = codecData
            pendingCodecType = codecType; pendingCodecData = codecData
            return
        }
        // iPhone 回应 forceKeyFrame 时会重发同一份 avcC；配置没变就别重建解码器，避免周期性黑帧/卡顿
        if (codec != null && lastCodecType == codecType && this.width == width && this.height == height
            && lastCodecData != null && lastCodecData!!.contentEquals(codecData)) {
            Log.i("AllPlay", "MediaCodec 配置未变，跳过重建")
            return
        }
        this.width = width
        this.height = height
        this.mime = if (codecType == 1) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC
        lastCodecType = codecType
        lastCodecData = codecData
        release()
        val s = surface
        if (s == null) {
            pendingCodecType = codecType
            pendingCodecData = codecData
            return
        }
        try {
            this.codec = MediaCodec.createDecoderByType(mime)
        } catch (e: Exception) {
            this.codec = null
            return
        }
        val format = MediaFormat.createVideoFormat(mime, width, height)
        val csd = buildCsd(codecData)
        csd.forEachIndexed { i, bytes -> format.setByteBuffer("csd-${i}", ByteBuffer.wrap(bytes)) }
        try {
            this.codec!!.configure(format, s, null, 0)
            this.codec!!.start()
        } catch (e: Exception) {
            // 退后台/旋转时 Surface 可能已被释放，configure/start 会失败；
            // 别让异常经 JNI 传导崩掉原生线程，退避 1s 后重试，暂存配置等新 Surface
            Log.w("AllPlay", "MediaCodec configure 失败（Surface 可能已释放）：${e.message}")
            configureFailUntil = System.currentTimeMillis() + 1000
            this.codec = null
            pendingCodecType = codecType
            pendingCodecData = codecData
            return
        }
        Log.i("AllPlay", "MediaCodec 已配置并启动 mime=$mime ${width}x${height} csd=${csd.size} surface=${s != null}")
    }

    /// 退后台 Surface 销毁时调用：清掉对已释放 Surface 的引用，避免后续 configure 崩溃
    fun releaseSurface() {
        Log.i("AllPlay", "MediaCodec releaseSurface")
        surface = null
        release()
    }

    fun setSurface(s: Surface) {
        surface = s
        Log.i("AllPlay", "MediaCodec setSurface surface=${s != null}")
        // surface 后补：之前因 surface 未就绪而暂存的配置，现在补配
        val d = pendingCodecData
        if (d != null) {
            pendingCodecData = null
            configure(pendingCodecType, width, height, d)
            return
        }
        // 已有解码器但 Surface 变了（后台→前台 Surface 重建）：优先用 setOutputSurface 热切换，
        // 失败则用最近一次配置重建（参照 setOutputSurface → reconfigure 兜底）
        val c = codec
        if (c != null) {
            // setOutputSurface 是 API 23；4.4 上直接按最近一次配置重建解码器
            if (Build.VERSION.SDK_INT >= 23) {
                try {
                    c.setOutputSurface(s)
                } catch (e: Exception) {
                    lastCodecData?.let { configure(lastCodecType, width, height, it) }
                }
            } else {
                lastCodecData?.let { configure(lastCodecType, width, height, it) }
            }
        } else {
            // 解码器在退后台时因 Surface 销毁被释放；Surface 回来用最近配置重建
            lastCodecData?.let { configure(lastCodecType, width, height, it) }
        }
    }

    fun feedAnnexB(data: ByteArray): Boolean {
        val c = codec ?: return false
        try {
            // VideoInputPump：边清输出边等输入缓冲（对照 VideoInputPump 模式）。
            // 若输出缓冲被占满，输入会被饿死 → dequeueInputBuffer 一直拿不到 → 静默丢帧 → 参考链断裂花屏。
            // 这里在 500ms 窗口内反复 drain 输出，把输入腾出来，而不是直接丢这一帧。
            var idx = -1
            val deadline = System.nanoTime() + 500_000_000L
            while (idx < 0 && System.nanoTime() < deadline) {
                idx = c.dequeueInputBuffer(0)
                if (idx >= 0) break
                drain()
            }
            if (idx < 0) return false
            val buf = c.compatInputBuffer(idx) ?: return false
            buf.clear()
            if (data.size > buf.remaining()) return false   // 超大帧：参考链已断，交给上层重放闸 + 重配
            buf.put(data)
            // 时间戳必须单调递增，否则解码器按错序渲染（乱码）。参照协议用 System.nanoTime()/1000
            c.queueInputBuffer(idx, 0, data.size, System.nanoTime() / 1000, 0)
            drain()
            return true
        } catch (e: Exception) {
            // 退后台 Surface 销毁会让 MediaCodec 进入非法态，dequeueInput/Output 抛 IllegalStateException；
            // 若让异常经 JNI 传导会崩掉原生屏幕流线程 → CarPlay 整条断连。这里吞掉、释放解码器、返回 false，
            // core 会放闸等关键帧，Surface 回来后 setSurface 再用最近配置重建。
            Log.w("AllPlay", "视频喂帧异常，释放解码器等待 Surface 重建：${e.message}")
            release()
            return false
        }
    }

    fun reset() {
        release()
    }

    fun flush() {
        try { codec?.flush() } catch (_: Exception) {}
    }

    private var decodedFrames = 0
    @Volatile var renderedFrames = 0
        private set

    private fun drain() {
        val c = codec ?: return
        val info = MediaCodec.BufferInfo()
        while (true) {
            val out = c.dequeueOutputBuffer(info, 0)
            when {
                out >= 0 -> {
                    // Surface 在后台可能已被销毁，render=true 会抛异常；失败则退化为不渲染，
                    // 避免异常经 JNI 传导弄挂原生线程（退后台后 CarPlay 整条断连的诱因之一）
                    var rendered = false
                    try {
                        c.releaseOutputBuffer(out, true)   // 交给 Surface 合成
                        rendered = true
                    } catch (e: Exception) {
                        try { c.releaseOutputBuffer(out, false) } catch (_: Exception) {}
                    }
                    if (rendered) renderedFrames++
                    decodedFrames++
                    if (decodedFrames == 1 || decodedFrames % 300 == 0)
                        Log.i("AllPlay", "MediaCodec 输出帧 #$decodedFrames size=${info.size}")
                }
                out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED ->
                    Log.i("AllPlay", "MediaCodec 输出格式变更 ${c.outputFormat}")
                out == MediaCodec.INFO_TRY_AGAIN_LATER -> return
                else -> Unit
            }
            if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) return
        }
    }

    private fun release() {
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        codec = null
    }

    // avcC/hvcC → MediaCodec csd-0/csd-1（AnnexB，带 start code）。参照协议 MediaCodecSupport。
    private fun buildCsd(codecData: ByteArray): List<ByteArray> {
        if (mime == MediaFormat.MIMETYPE_VIDEO_AVC) {
            // avcC：SPS 数量在 [5]&0x1f，offset 6 起 u16BE 长度 + SPS；随后 PPS
            if (codecData.size < 7) return emptyList()
            val spsCount = codecData[5].toInt() and 0x1f
            var off = 6
            val spsList = ArrayList<ByteArray>()
            repeat(spsCount) {
                if (off + 2 > codecData.size) return@repeat
                val len = ((codecData[off].toInt() and 0xff) shl 8) or (codecData[off + 1].toInt() and 0xff)
                off += 2
                spsList.add(codecData.copyOfRange(off, off + len))
                off += len
            }
            if (off >= codecData.size) return emptyList()
            val ppsCount = codecData[off++].toInt() and 0xff
            val ppsList = ArrayList<ByteArray>()
            repeat(ppsCount) {
                if (off + 2 > codecData.size) return@repeat
                val len = ((codecData[off].toInt() and 0xff) shl 8) or (codecData[off + 1].toInt() and 0xff)
                off += 2
                ppsList.add(codecData.copyOfRange(off, off + len))
                off += len
            }
            val start = byteArrayOf(0, 0, 0, 1)
            return listOfNotNull(
                spsList.firstOrNull()?.let { start + it },
                ppsList.firstOrNull()?.let { start + it },
            )
        } else {
            // HEVC hvcC：VPS/SPS/PPS 各数组（array count 在 offset 22）。首版只取第一个 NAL 集。
            return listOf(byteArrayOf(0, 0, 0, 1) + codecData)   // TODO(真机)：按 hvcC 逐数组拆 VPS/SPS/PPS
        }
    }
}
