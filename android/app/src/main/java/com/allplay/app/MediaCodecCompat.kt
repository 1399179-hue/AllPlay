package com.allplay.app

import android.media.MediaCodec
import android.os.Build
import java.nio.ByteBuffer

/**
 * MediaCodec 输入/输出 buffer 访问的 API 19 兼容层。
 * API 21+ 用 getInputBuffer(idx)/getOutputBuffer(idx)；Android 4.4（API 19）只有
 * 数组形式的 getInputBuffers()/getOutputBuffers()（已弃用但仍可用）。
 */
internal fun MediaCodec.compatInputBuffer(idx: Int): ByteBuffer? =
    if (Build.VERSION.SDK_INT >= 21) getInputBuffer(idx)
    else @Suppress("DEPRECATION") getInputBuffers()[idx]

internal fun MediaCodec.compatOutputBuffer(idx: Int): ByteBuffer? =
    if (Build.VERSION.SDK_INT >= 21) getOutputBuffer(idx)
    else @Suppress("DEPRECATION") getOutputBuffers()[idx]
