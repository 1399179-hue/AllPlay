// ============================================================================
// jni_bridge.cpp —— AllPlay 原生桥：把 core::port 接口实现成对 Kotlin 服务的下调
// ============================================================================
// core 跑在原生线程（RTSP/屏幕流）。蓝牙 / mDNS / 视频解码经 JNI 下调到
// Kotlin 的 BluetoothService / MdnsService / MediaCodecSink；TCP 与网卡复用
// platform/posix/net_posix.cpp（NDK 提供 POSIX socket + getifaddrs）。
//
// 线程约定：JavaVM 指针在 JNI_OnLoad 拿；core 线程下调前 AttachCurrentThread。
// 服务对象（bt/mdns/video）用 NewGlobalRef 保活到 nativeStop。
#include <jni.h>

#include "airplay/server.h"
#include "bt/bt_trigger.h"
#include "port/bluetooth.h"
#include "port/mdns.h"
#include "port/media.h"
#include "port/platform.h"
#include "posix/posix.h"

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AllPlay", __VA_ARGS__)

// net_posix.cpp 的 makePlatformServices 会引用 allplay::posix::makeMdnsAdvertiser；
// Android 不编译 posix 的 mdns/bt 实现，这里给个桩满足链接（实际用 jni_bridge 自组的 services）。
namespace allplay::posix {
std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser() { return nullptr; }
}

namespace {

JavaVM* g_vm = nullptr;
jobject g_bt    = nullptr;   // BluetoothService（global ref）
jobject g_mdns  = nullptr;   // MdnsService
jobject g_video = nullptr;   // MediaCodecSink
jobject g_audio = nullptr;   // AudioSink

std::unique_ptr<core::airplay::Receiver> g_receiver;
core::PlatformServices g_services;
core::airplay::ReceiverConfig g_cfg;

/// nativeStop 置位，让 detach 的蓝牙触发线程自己退出（它不能再碰已被销毁的对象）。
std::atomic<bool> g_btTriggerStop{false};

/// 拿当前线程的 JNIEnv（core 线程要 attach）。
JNIEnv* envOf(bool* attached)
{
    *attached = false;
    JNIEnv* env = nullptr;
    if (!g_vm) return nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) *attached = true;
    return env;
}

std::string jstr(JNIEnv* env, jstring s)
{
    if (!s) return {};
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string out = p ? p : "";
    if (p) env->ReleaseStringUTFChars(s, p);
    return out;
}

jstring newJstr(JNIEnv* env, const std::string& s)
{
    return env->NewStringUTF(s.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
/// 把 BluetoothService 的 handle 包成 core::IByteStream（read/write/close 下调 Kotlin）。
class JniByteStream : public core::IByteStream {
public:
    explicit JniByteStream(int handle) : handle_(handle) {}
    ~JniByteStream() override { close(); }

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        if (closed_.load()) return -1;   // 已被 nativeStop/重连关闭：当作断开，绝不碰已释放对象
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_bt) return -1;
        jclass cls = env->GetObjectClass(g_bt);
        jmethodID m = env->GetMethodID(cls, "read", "(III)[B");
        env->DeleteLocalRef(cls);
        if (!m) return -1;
        auto arr = static_cast<jbyteArray>(env->CallObjectMethod(g_bt, m, jint(handle_.load()),
                                                                 jint(n), jint(timeoutMs)));
        if (!arr) { if (attached) g_vm->DetachCurrentThread(); return -1; }   // null=断开
        const jsize len = env->GetArrayLength(arr);
        if (len == 0) { env->DeleteLocalRef(arr); if (attached) g_vm->DetachCurrentThread(); return 0; }  // 空=超时
        env->GetByteArrayRegion(arr, 0, len, reinterpret_cast<jbyte*>(buf));
        env->DeleteLocalRef(arr);
        if (attached) g_vm->DetachCurrentThread();
        return int(len);
    }

    bool writeAll(const uint8_t* buf, size_t n) override
    {
        if (closed_.load()) return false;
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_bt) return false;
        jclass cls = env->GetObjectClass(g_bt);
        jmethodID m = env->GetMethodID(cls, "write", "(I[B)Z");
        env->DeleteLocalRef(cls);
        if (!m) return false;
        jbyteArray arr = env->NewByteArray(jsize(n));
        if (n) env->SetByteArrayRegion(arr, 0, jsize(n), reinterpret_cast<const jbyte*>(buf));
        const bool ok = env->CallBooleanMethod(g_bt, m, jint(handle_.load()), arr);
        if (arr) env->DeleteLocalRef(arr);
        if (attached) g_vm->DetachCurrentThread();
        return ok;
    }

    void close() override
    {
        if (closed_.exchange(true)) return;   // 幂等
        const int handle = handle_.exchange(-1);
        if (handle < 0) return;
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_bt) return;
        jclass cls = env->GetObjectClass(g_bt);
        jmethodID m = env->GetMethodID(cls, "close", "(I)V");
        env->DeleteLocalRef(cls);
        if (m) env->CallVoidMethod(g_bt, m, jint(handle));
        if (attached) g_vm->DetachCurrentThread();
    }

private:
    std::atomic<int> handle_;
    std::atomic<bool> closed_{false};
};

// JniBluetooth —— IBluetooth：下调 Kotlin BluetoothService
// ---------------------------------------------------------------------------
class JniBluetooth : public core::IBluetooth {
public:
    ~JniBluetooth() override { closeStream(); }

    std::vector<core::BtDevice> pairedDevices(std::string* err) override
    {
        std::vector<core::BtDevice> out;
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_bt) return out;
        jclass cls = env->GetObjectClass(g_bt);
        jmethodID m = env->GetMethodID(cls, "pairedDevices", "()[Ljava/lang/String;");
        env->DeleteLocalRef(cls);
        if (!m) return out;
        auto* arr = static_cast<jobjectArray>(env->CallObjectMethod(g_bt, m));
        if (!arr) return out;
        const jsize n = env->GetArrayLength(arr);
        for (jsize i = 0; i < n; ++i) {
            auto s = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
            const std::string v = jstr(env, s);
            if (s) env->DeleteLocalRef(s);
            const size_t bar = v.find('|');
            core::BtDevice d;
            d.name = bar == std::string::npos ? v : v.substr(0, bar);
            d.address = bar == std::string::npos ? "" : v.substr(bar + 1);
            out.push_back(std::move(d));
        }
        env->DeleteLocalRef(arr);
        if (attached) g_vm->DetachCurrentThread();
        return out;
    }

    core::IByteStream* connectRFCOMM(const std::string& address, const std::string& uuid,
                                     std::string* err) override
    {
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_bt) return nullptr;
        jclass cls = env->GetObjectClass(g_bt);
        jmethodID m = env->GetMethodID(cls, "connectSpp",
                                       "(Ljava/lang/String;Ljava/lang/String;)I");
        env->DeleteLocalRef(cls);
        if (!m) return nullptr;
        const jint h = env->CallIntMethod(g_bt, m, newJstr(env, address), newJstr(env, uuid));
        if (attached) g_vm->DetachCurrentThread();
        if (h < 0) { if (err) *err = "connectSpp 失败"; return nullptr; }
        // 关掉上一条 RFCOMM：iPhone 的 iAP2 SPP 服务只接受一条连接，重连前必须先关旧的
        closeStream();
        currentStream_ = new JniByteStream(h);
        return currentStream_;
    }

    core::IByteStream* waitForPhoneConnection(int timeoutMs, std::string* err) override
    {
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_bt) return nullptr;
        jclass cls = env->GetObjectClass(g_bt);
        jmethodID m = env->GetMethodID(cls, "listenSpp", "(Ljava/lang/String;I)I");
        env->DeleteLocalRef(cls);
        if (!m) return nullptr;
        const jint h = env->CallIntMethod(g_bt, m, newJstr(env, core::kWirelessIapUuid), jint(timeoutMs));
        if (attached) g_vm->DetachCurrentThread();
        if (h < 0) { if (err) *err = "listenSpp 失败"; return nullptr; }
        closeStream();
        currentStream_ = new JniByteStream(h);
        return currentStream_;
    }

private:
    void closeStream()
    {
        if (!currentStream_) return;
        currentStream_->close();
        // 蓝牙触发线程是 detach 的，可能仍持有这个指针；直接 delete 会在 nativeStop/重连时
        // 变成 use-after-free（真机 SIGSEGV 在 WirelessTrigger::run 里就是这么来的）。
        // 关闭后保留对象到进程结束：数量等于重连次数，每个几十字节，换来的是不会崩。
        retiredStreams_.push_back(currentStream_);
        currentStream_ = nullptr;
    }

    core::IByteStream* currentStream_ = nullptr;
    std::vector<core::IByteStream*> retiredStreams_;
};

// ---------------------------------------------------------------------------
// JniMdns —— IMdnsAdvertiser：下调 Kotlin MdnsService（NsdManager）
// ---------------------------------------------------------------------------
class JniMdns : public core::IMdnsAdvertiser {
public:
    bool start(const std::string& name, uint16_t port,
               const std::vector<std::string>& txt, std::string* err) override
    {
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_mdns) return false;
        jclass cls = env->GetObjectClass(g_mdns);
        jmethodID m = env->GetMethodID(cls, "register",
                                       "(Ljava/lang/String;I[Ljava/lang/String;)Z");
        env->DeleteLocalRef(cls);
        if (!m) return false;
        auto* arr = env->NewObjectArray(jsize(txt.size()), env->FindClass("java/lang/String"), nullptr);
        for (size_t i = 0; i < txt.size(); ++i)
            env->SetObjectArrayElement(arr, jsize(i), newJstr(env, txt[i]));
        const bool ok = env->CallBooleanMethod(g_mdns, m, newJstr(env, name), jint(port), arr);
        env->DeleteLocalRef(arr);
        if (attached) g_vm->DetachCurrentThread();
        if (!ok && err) *err = "NsdManager.registerService 失败";
        return ok;
    }
    void stop() override { /* NsdManager 在 nativeStop 统一注销 */ }
};

// ---------------------------------------------------------------------------
// JniVideo —— IVideoOutput：下调 Kotlin MediaCodecSink（MediaCodec → Surface）
// ---------------------------------------------------------------------------
class JniVideo : public core::IVideoOutput {
public:
    void configure(core::VideoCodec codec, int width, int height,
                   const std::vector<uint8_t>& codecData) override
    {
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_video) return;
        jclass cls = env->GetObjectClass(g_video);
        jmethodID m = env->GetMethodID(cls, "configure", "(III[B)V");
        env->DeleteLocalRef(cls);
        if (!m) return;
        jbyteArray arr = env->NewByteArray(jsize(codecData.size()));
        if (codecData.size()) env->SetByteArrayRegion(arr, 0, jsize(codecData.size()),
                                                      reinterpret_cast<const jbyte*>(codecData.data()));
        env->CallVoidMethod(g_video, m, jint(codec == core::VideoCodec::HEVC ? 1 : 0),
                            jint(width), jint(height), arr);
        if (arr) env->DeleteLocalRef(arr);
        if (attached) g_vm->DetachCurrentThread();
    }

    bool feedAnnexB(const uint8_t* data, size_t len) override
    {
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_video) return false;
        jclass cls = env->GetObjectClass(g_video);
        jmethodID m = env->GetMethodID(cls, "feedAnnexB", "([B)Z");
        env->DeleteLocalRef(cls);
        if (!m) { if (attached) g_vm->DetachCurrentThread(); return false; }
        jbyteArray arr = env->NewByteArray(jsize(len));
        if (len) env->SetByteArrayRegion(arr, 0, jsize(len), reinterpret_cast<const jbyte*>(data));
        const bool ok = env->CallBooleanMethod(g_video, m, arr) == JNI_TRUE;
        if (arr) env->DeleteLocalRef(arr);
        if (attached) g_vm->DetachCurrentThread();
        return ok;
    }

    void reset() override
    {
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_video) return;
        jclass cls = env->GetObjectClass(g_video);
        jmethodID m = env->GetMethodID(cls, "reset", "()V");
        env->DeleteLocalRef(cls);
        if (m) env->CallVoidMethod(g_video, m);
        if (attached) g_vm->DetachCurrentThread();
    }

    void flush() override
    {
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_video) return;
        jclass cls = env->GetObjectClass(g_video);
        jmethodID m = env->GetMethodID(cls, "flush", "()V");
        env->DeleteLocalRef(cls);
        if (m) env->CallVoidMethod(g_video, m);
        if (attached) g_vm->DetachCurrentThread();
    }
};

// ---------------------------------------------------------------------------
// JniAudio —— IAudioOutput：下调 Kotlin AudioSink（Opus/AAC 解码 + AudioTrack）
// ---------------------------------------------------------------------------
class JniAudio : public core::IAudioOutput {
public:
    void configure(core::AudioCodec codec, int sampleRate, int channels) override
    {
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_audio) return;
        jclass cls = env->GetObjectClass(g_audio);
        jmethodID m = env->GetMethodID(cls, "configure", "(III)V");
        env->DeleteLocalRef(cls);
        if (!m) return;
        const int c = codec == core::AudioCodec::OPUS ? 1 :
                      codec == core::AudioCodec::AAC_LC ? 0 : 2;
        env->CallVoidMethod(g_audio, m, jint(c), jint(sampleRate), jint(channels));
        if (attached) g_vm->DetachCurrentThread();
    }

    void feed(const uint8_t* payload, size_t len, uint32_t sample) override
    {
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_audio) return;
        jclass cls = env->GetObjectClass(g_audio);
        jmethodID m = env->GetMethodID(cls, "feed", "([BI)V");
        env->DeleteLocalRef(cls);
        if (!m) return;
        jbyteArray arr = env->NewByteArray(jsize(len));
        if (len) env->SetByteArrayRegion(arr, 0, jsize(len), reinterpret_cast<const jbyte*>(payload));
        env->CallVoidMethod(g_audio, m, arr, jint(sample));
        if (arr) env->DeleteLocalRef(arr);
        if (attached) g_vm->DetachCurrentThread();
    }

    void conceal(size_t frames) override
    {
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_audio) return;
        jclass cls = env->GetObjectClass(g_audio);
        jmethodID m = env->GetMethodID(cls, "conceal", "(I)V");
        env->DeleteLocalRef(cls);
        if (m) env->CallVoidMethod(g_audio, m, jint(frames));
        if (attached) g_vm->DetachCurrentThread();
    }

    void reset() override
    {
        bool attached = false;
        JNIEnv* env = envOf(&attached);
        if (!env || !g_audio) return;
        jclass cls = env->GetObjectClass(g_audio);
        jmethodID m = env->GetMethodID(cls, "reset", "()V");
        env->DeleteLocalRef(cls);
        if (m) env->CallVoidMethod(g_audio, m);
        if (attached) g_vm->DetachCurrentThread();
    }
};

// ---------------------------------------------------------------------------
// 组合根：装配 PlatformServices 并起 Receiver
// ---------------------------------------------------------------------------
static JniBluetooth* s_bt = nullptr;
static JniMdns*       s_mdns = nullptr;
static JniVideo*      s_video = nullptr;
static JniAudio*      s_audio = nullptr;

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*)
{
    g_vm = vm;
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL
Java_com_allplay_app_NativeAllPlay_nativeStart(JNIEnv* env, jobject,
                                               jobject bt, jobject mdns, jobject video, jobject audio,
                                               jstring stateDir, jstring identityDir,
                                               jstring name, jstring btMac, jstring ssid,
                                               jstring passphrase, jstring ip,
                                               jint width, jint height, jboolean skipBtTrigger,
                                               jint fps)
{
    g_bt    = env->NewGlobalRef(bt);
    g_mdns  = env->NewGlobalRef(mdns);
    g_video = env->NewGlobalRef(video);
    g_audio = env->NewGlobalRef(audio);

    s_bt = new JniBluetooth;
    s_mdns = new JniMdns;
    s_video = new JniVideo;
    s_audio = new JniAudio;

    g_services.network = &allplay::posix::networkInfo();
    g_services.makeTcpListener = &allplay::posix::makeTcpListener;
    g_services.makeMdnsAdvertiser = [] { return std::unique_ptr<core::IMdnsAdvertiser>(new JniMdns); };
    g_services.makeUdpSocket = &allplay::posix::makeUdpSocket;
    g_services.bluetooth = s_bt;

    g_cfg.name = jstr(env, name);
    g_cfg.stateDir = jstr(env, stateDir);
    g_cfg.mfiDir = jstr(env, identityDir);
    g_cfg.btMac = jstr(env, btMac);
    g_cfg.width = int(width);
    g_cfg.height = int(height);
    g_cfg.fps = int(fps);
    g_cfg.video = s_video;
    g_cfg.audio = s_audio;

    const std::string btMacStr = jstr(env, btMac);
    const std::string ssidStr  = jstr(env, ssid);
    const std::string passStr  = jstr(env, passphrase);
    const std::string ipStr    = jstr(env, ip);

    g_receiver = std::make_unique<core::airplay::Receiver>();
    std::string err;
    if (!g_receiver->start(g_cfg, g_services, &err)) return -1;
    const jint port = jint(g_receiver->port());
    g_btTriggerStop.store(false);   // 新会话：允许蓝牙触发线程工作
    LOGI("nativeStart ok: port=%d ip=%s deviceid=%s btMac=%s ssid=%s",
         int(port), g_receiver->ip().c_str(), g_receiver->info().deviceId.c_str(),
         btMacStr.c_str(), ssidStr.c_str());

    // 蓝牙触发线程：连 iPhone 的 iAP2（RFCOMM）→ 0x4301（手机随后走 Wi-Fi 来连 AirPlay）
    // 旋转重启时 skipBtTrigger=true：iPhone 已在同一 WiFi，直接靠 mDNS + carplay-probe 快速重连，别重走慢速蓝牙握手
    if (!skipBtTrigger) {
        std::thread([btMacStr, ssidStr, passStr, ipStr] {
            core::bt::BtTriggerParams bp;
            bp.deviceName = "iPhone";
            bp.serverMode = false;                   // 客户端模式：主动连手机的 iAP2 SPP 服务（协议主路径）
            bp.serverWaitMs = 180000;
            auto& c = bp.iap2;
            c.name = g_cfg.name;
            c.model = "AllPlay1,1";
            c.manufacturer = "AllPlay";
            c.serial = g_receiver->info().deviceId;
            c.firmware = "1.0.0";
            c.hardware = "1.0";
            c.btMac = btMacStr;
            c.ssid = ssidStr;
            c.passphrase = passStr;
            c.channel = 0;
            c.securityType = passStr.empty() ? 0 : 2;   // 有口令→WPA2；无口令→NONE
            c.ipAddresses = {ipStr.empty() ? g_receiver->ip() : ipStr};   // 热点模式用热点接口 IP
            c.airPlayPort = g_receiver->port();
            c.deviceIdentifier = g_receiver->info().deviceId;
            c.publicKeyHex = g_receiver->info().pkHex;
            c.sourceVersion = core::airplay::kSourceVersion;
            c.mfiDir = g_cfg.mfiDir;
            c.timeoutMs = 180000;

            LOGI("trigger: ssid=%s securityType=%d passphraseLen=%zu ip=%s",
                 c.ssid.c_str(), c.securityType, c.passphrase.size(),
                 c.ipAddresses.empty() ? "?" : c.ipAddresses[0].c_str());

            // iPhone 的 iAP2 SPP 服务在蓝牙 ACL 连上后才异步打开，单次 connectSpp 极易太早失败。
            // 整链重试（枚举→RFCOMM→iAP2→0x4301），直到发出 0x4301。
            // 发出后等 AirPlay 会话断开（如 iPhone 手动断开 WiFi），断开后重新触发——否则
            // iPhone 重连 WiFi 后不会自动重进 CarPlay。
            constexpr int kTriggerAttempts = 10;
            constexpr int kRetryDelayMs    = 3000;
            for (;;) {
                if (g_btTriggerStop.load()) {
                    LOGI("bt trigger 线程退出（nativeStop）");
                    return;
                }
                bool triggered = false;
                for (int attempt = 1; attempt <= kTriggerAttempts; ++attempt) {
                    auto r = core::bt::runBtTrigger(s_bt, bp, [](const std::string& s) { LOGI("[bt] %s", s.c_str()); });
                    if (r.ok) {
                        LOGI("bt trigger 成功（第 %d 次尝试）", attempt);
                        triggered = true;
                        break;
                    }
                    LOGI("bt trigger 第 %d/%d 次失败：%s", attempt, kTriggerAttempts, r.error.c_str());
                    if (attempt < kTriggerAttempts)
                        std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelayMs));
                }
                if (!triggered) {
                    LOGI("bt trigger 重试 %d 次后仍失败，放弃", kTriggerAttempts);
                    break;
                }
                // 等 AirPlay 会话建立（sessionActive → true），最多 60 秒
                bool becameActive = false;
                for (int i = 0; i < 60; ++i) {
                    if (g_btTriggerStop.load()) return;
                    if (g_receiver && g_receiver->sessionActive()) { becameActive = true; break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                }
                if (!becameActive) {
                    LOGI("AirPlay 会话 60 秒内未建立，重新触发蓝牙");
                    continue;
                }
                // 等 AirPlay 会话断开（sessionActive → false），再重新触发
                while (g_receiver && g_receiver->sessionActive()) {
                    if (g_btTriggerStop.load()) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                }
                LOGI("AirPlay 会话已断开，重新触发蓝牙");
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }).detach();
    } else {
        LOGI("跳过蓝牙触发（旋转重启）：走 mDNS + carplay-probe 快速重连");
    }

    return port;
}

JNIEXPORT void JNICALL
Java_com_allplay_app_NativeAllPlay_nativeStop(JNIEnv* env, jobject)
{
    g_btTriggerStop.store(true);   // 先让触发线程退出，再拆对象
    if (g_receiver) { g_receiver->stop(); g_receiver.reset(); }
    delete s_bt;    s_bt = nullptr;
    delete s_mdns;  s_mdns = nullptr;
    delete s_video; s_video = nullptr;
    delete s_audio; s_audio = nullptr;
    if (g_bt)    { env->DeleteGlobalRef(g_bt);    g_bt = nullptr; }
    if (g_mdns)  { env->DeleteGlobalRef(g_mdns);  g_mdns = nullptr; }
    if (g_video) { env->DeleteGlobalRef(g_video); g_video = nullptr; }
    if (g_audio) { env->DeleteGlobalRef(g_audio); g_audio = nullptr; }
}

JNIEXPORT jstring JNICALL
Java_com_allplay_app_NativeAllPlay_nativeDeviceId(JNIEnv* env, jobject)
{
    if (!g_receiver) return env->NewStringUTF("");
    return env->NewStringUTF(g_receiver->info().deviceId.c_str());
}

JNIEXPORT void JNICALL
Java_com_allplay_app_NativeAllPlay_nativeSetSurface(JNIEnv* env, jobject, jobject surface)
{
    if (!g_video) return;
    jclass cls = env->GetObjectClass(g_video);
    jmethodID m = env->GetMethodID(cls, "setSurface", "(Landroid/view/Surface;)V");
    env->DeleteLocalRef(cls);
    if (m) env->CallVoidMethod(g_video, m, surface);
}

JNIEXPORT void JNICALL
Java_com_allplay_app_NativeAllPlay_nativeSendTouch(JNIEnv* env, jobject, jfloatArray points)
{
    if (!g_receiver) return;
    const jsize n = env->GetArrayLength(points);
    jfloat* p = env->GetFloatArrayElements(points, nullptr);
    if (!p) return;
    std::vector<core::airplay::TouchPoint> contacts;
    for (jsize i = 0; i + 2 < n; i += 3) {
        core::airplay::TouchPoint t;
        t.x    = p[i];
        t.y    = p[i + 1];
        t.down = p[i + 2] != 0.0f;
        contacts.push_back(t);
    }
    env->ReleaseFloatArrayElements(points, p, JNI_ABORT);
    g_receiver->sendTouch(contacts);
}

JNIEXPORT void JNICALL
Java_com_allplay_app_NativeAllPlay_nativeRequestKeyFrame(JNIEnv* env, jobject)
{
    if (g_receiver) g_receiver->requestKeyFrameNow();
}

JNIEXPORT jint JNICALL
Java_com_allplay_app_NativeAllPlay_nativeSessionState(JNIEnv* env, jobject)
{
    if (!g_receiver) return 0;
    if (g_receiver->sessionActive()) return 2;   // 已连接（RECORD 后）
    if (g_receiver->connecting())   return 1;   // 连接中（收到 RTSP 请求但未 RECORD）
    return 0;                                    // 等待连接
}

}  // extern "C"
