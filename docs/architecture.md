# AllPlay 架构框架图

> 图用 Mermaid 绘制，GitHub / VS Code（装 Mermaid 插件）/ Typora 都能直接渲染。

## 一、分层架构（ports & adapters）

```mermaid
flowchart TB
    subgraph HOST["宿主层（组合根 / Composition Root）"]
        direction LR
        ANDROID["Android App<br/>MainActivity · AllPlaySessionService<br/>MediaCodecSink · AudioSink<br/>BluetoothService · MdnsService · HotspotManager"]
        DESKTOP["桌面 Host<br/>macOS/Linux: SDL2<br/>Windows: Win32 窗口"]
    end

    subgraph ADAPTER["平台适配层（实现 core::port 接口）"]
        direction LR
        POSIX["platform/posix<br/>net_posix · mdns(avahi/dns_sd)<br/>bt(IOBluetooth/BlueZ)"]
        WIN["platform/windows<br/>net_windows(Winsock2)<br/>bt_windows(AF_BTH)<br/>mdns"]
        JNI["android JNI<br/>jni_bridge.cpp"]
    end

    subgraph PORT["接口层（core/port，平台无关）"]
        direction LR
        TCP["ITcpListener / ITcpStream"]
        UDP["IUdpSocket"]
        NET["INetworkInfo"]
        BT["IBluetooth"]
        MDNS["IMdnsAdvertiser"]
        MEDIA["IVideoOutput / IAudioOutput"]
        STREAM["IByteStream"]
        PS["PlatformServices"]
    end

    subgraph CORE["协议核心（core，平台无关，唯一三方依赖 = OpenSSL）"]
        direction LR
        AIRPLAY["airplay<br/>rtsp · server · session<br/>video_stream · audio_stream<br/>event_channel · control_cipher · info"]
        IAP2["iap2<br/>wire · link · wireless_client"]
        MFI["mfi<br/>mfi_identity"]
        CRYPTO["crypto<br/>hkdf · chacha20poly1305 · srp6a<br/>x25519 · ed25519 · tlv8 · aes_ctr"]
        BPLIST["bplist"]
        BTTRIG["bt<br/>bt_trigger"]
        IDENTITY["platform<br/>device_identity · pairing_store"]
        COMMON["common<br/>log"]
    end

    HOST -->|注入实现| ADAPTER
    ADAPTER -->|实现| PORT
    CORE -->|依赖| PORT
    HOST -->|装配 PlatformServices| CORE
```

## 二、无线 CarPlay 数据流

```mermaid
flowchart LR
    IPHONE["iPhone<br/>(CarPlay 源端)"]

    subgraph RECEIVER["AllPlay 接收端"]
        BT_TRIG["蓝牙 iAP2 触发<br/>RFCOMM → 0x4301"]
        RTSP["RTSP 控制通道 :7000<br/>pair-setup / pair-verify<br/>auth-setup / SETUP / RECORD"]
        SCREEN["屏幕流 110/111<br/>TCP · H.264 · AVCC→AnnexB"]
        AUDIO["音频流 100/101/102<br/>UDP RTP · AAC/Opus/LPCM"]
        EVENT["事件通道 eventPort<br/>触摸/命令 · ControlCipher 加密"]
        MDNS_B["mDNS 广播<br/>_airplay._tcp"]
    end

    IPHONE -->|"① 蓝牙连入，发 iAP2"| BT_TRIG
    BT_TRIG -->|"② 0x4301 引导（SSID/密码/IP/端口）"| IPHONE
    IPHONE -->|"③ 连 Wi-Fi 后经 mDNS 发现"| MDNS_B
    IPHONE -->|"④ RTSP 协商"| RTSP
    RTSP -->|"⑤ 建立数据流"| SCREEN
    RTSP -->|"⑤ 建立音频流"| AUDIO
    RTSP -->|"⑤ 建立事件通道"| EVENT
    SCREEN -->|"H.264 帧 → IVideoOutput 解码上屏"| SCREEN
    EVENT -->|"触摸 → hidSendReport → 回 iPhone"| IPHONE
```

## 三、核心模块职责

| 模块 | 职责 |
|---|---|
| `airplay/rtsp` | 极简 RTSP 服务器（控制通道，pair-verify 后加密） |
| `airplay/session` | 会话状态机：pair-setup / pair-verify / auth-setup / SETUP |
| `airplay/video_stream` | 屏幕流：128B 头解密 → AVCC→AnnexB → 关键帧闸门 |
| `airplay/audio_stream` | 音频流：RTP over UDP 解密 + RTP 序号乱序重排 |
| `airplay/event_channel` | 事件通道：触摸 HID 回传 + forceKeyFrame 命令 |
| `iap2/wireless_client` | 蓝牙侧 iAP2 控制流程（0xAA00~0xAA05 / 0x1D00 / 0x5703 / 0x4301） |
| `mfi/mfi_identity` | MFi 配件身份（证书 + ECDSA 签名，接口可换硬件芯片） |
| `crypto/*` | 全部加密原语从零实现（不依赖第三方协议库） |
| `bt/bt_trigger` | 蓝牙触发编排（选设备 → RFCOMM → iAP2 → 0x4301） |

## 四、平台适配矩阵

| 能力 | macOS | Linux | Android | Windows |
|---|---|---|---|---|
| TCP/UDP | POSIX socket | POSIX socket | Socket | Winsock2 ✅ |
| 网卡信息 | getifaddrs | getifaddrs | NetworkInterface | GetAdaptersAddresses ✅ |
| 蓝牙 | IOBluetooth | BlueZ(待做) | BluetoothSocket | AF_BTH(待接入) |
| mDNS | dns_sd | avahi | NsdManager/JmDNS | 桩(待做) |
| 视频输出 | ffmpeg+SDL(待做) | ffmpeg+SDL(待做) | MediaCodec ✅ | ffmpeg(待做) |
