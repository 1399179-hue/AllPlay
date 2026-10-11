package com.allplay.app

import android.view.Surface

/**
 * 原生桥的 JNI 声明（对应 jni_bridge.cpp 里的
 * Java_com_allplay_app_NativeAllPlay_* 入口）。
 */
class NativeAllPlay {
    companion object {
        init {
            System.loadLibrary("allplay_jni")
        }
    }

    /// 启动 core（Receiver）：蓝牙/mDNS/视频三个服务对象交给 native 下调。
    /// @return RTSP 端口（>0 成功；-1 失败）
    external fun nativeStart(
        bt: BluetoothService,
        mdns: MdnsService,
        video: MediaCodecSink,
        audio: AudioSink,
        stateDir: String,
        identityDir: String,
        name: String,
        btMac: String,
        ssid: String,
        passphrase: String,
        ip: String,
        width: Int,
        height: Int,
        skipBtTrigger: Boolean,
        fps: Int,
    ): Int

    external fun nativeStop()

    external fun nativeSetSurface(surface: Surface)

    /// 发触摸：每组 3 个 float [x, y, down(0/1)]，最多 2 组；坐标归一化 0..1
    external fun nativeSendTouch(points: FloatArray)

    /// 立即向 iPhone 请求关键帧（回前台 Surface 重建后调用，尽快恢复画面）
    external fun nativeRequestKeyFrame()

    /// 会话状态：0=等待连接，1=连接中，2=已连接
    external fun nativeSessionState(): Int

    /// 本机 deviceid（如 "02:F5:92:44:8E:EC"），用于探测 /ctrl-int/1/connect
    external fun nativeDeviceId(): String
}
