/**
 * @file   wired_task.h
 * @brief  桌面宿主的有线（USB）CarPlay 接入任务
 * @author blt
 */

// ============================================================================
// host/wired_task.h —— 有线接入任务（宿主层）
// ============================================================================
// 跟 BtTask 是同一件事的两种接法，但**相位完全不同，别照抄**：
//
//   蓝牙：Receiver 先起来（绑通配 + mDNS 广播），触发只是告诉手机"来连我"。
//   有线：**Receiver 必须等 USB 侧备好了才能起** —— 手机是从 0x4301 里读地址和端口的，
//         而那个地址是 NCM 网卡上的 IPv6 链路本地，只有把 USB 拆开、按 bus/addr 认出
//         网卡、再把地址钉上去之后才知道。先起 Receiver 就得先猜地址，猜错等于把地址
//         说错（见 core/airplay/server.h 的 bindAddr 注释）。
//
// 所以这里分两段：
//
//   ① beginBringUp()（后台线程跑）：切 CarPlay 配置 → usbmux v2 → lockdown 配对 →
//      原地 TLS → carkit 服务 → iAP2 链路协商（主机发起）→ 识别 → MFi → 电源+订阅
//      → 备好 NCM 网卡。跑完 result() 里就是 ifname / bindAddr —— 正好是
//      ReceiverConfig 要的那两个字段。
//   ② 主线程拿这两个字段起 Receiver（**rx 的生命周期仍然只有一个线程在管**），
//      再 startPump() 让后台线程等 0x4300 → 回 0x4301。
//
// 为什么不把两段都塞进后台线程：主循环每帧都要读 rx（running/connecting/port/info…），
// 让另一个线程去调 rx.start() 就是实打实的数据竞争。
//
// 线程约定（对齐 bt_task.cpp）：两个线程都**只碰自己 move 进去的东西**（这里是一个
// shared_ptr<Impl> 加上按值快照的参数），绝不回头引用 this / rx / Options。
// 因此 stop() 里 join 是安全的，WiredTask 析构也不会把线程留在悬垂对象上。
//
// 平台：整条链路依赖 libusb（platform/posix/usb_libusb.cpp）。没编进 libusb 时
// beginBringUp() 会直接置 Failed 并给出原因，宿主照常跑无线那条路。
#pragma once

#include "airplay/server.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace allplay {

/// 日志回调（空则打到 stdout，前缀 [wired]）。和 BtTask 用同一个形状。
using WiredLogFn = std::function<void(const std::string&)>;

struct WiredTaskOptions {
    std::string name          = "AllPlay";   ///< iAP2 识别信息里报的车机名
    std::string mfiDir        = "identity";  ///< MFi 身份目录（identity.pk8 + certificate.p7b）
    /// 配对材料存哪。空 = defaultStateDir()。**sudo 会改 HOME**，跑在 root 下时必须显式给 ——
    /// 不然 root 存一份、blt 存一份，等于两台"电脑"，信任框还是每次弹。
    std::string stateDir;
    /// 钉在 NCM 网卡上的 IPv6 链路本地地址。空 = fe80::2（同 DiPlay 的 config.linkLocal）。
    /// **不要随便改**：手机是拿 0x4301 里这个地址来连的，改了只是换个值，但两端必须一致。
    std::string ncmLinkLocal;
    int         productId     = -1;          ///< -1 = 不限（苹果键盘/集线器会被自动排除）
    int         switchWaitMs  = 10000;       ///< 切配置后等设备回来的上限
};

/// ① 的结果：ReceiverConfig 要填的东西 + 几条只给日志看的。
struct WiredBringUp {
    std::string ifname;     ///< NCM 网卡名（enx… 由 MAC 来，所以不能写死）
    std::string bindAddr;   ///< 钉在它上面的链路本地地址 —— **原样填进 ReceiverConfig::bindAddr**
    std::string mac;        ///< 网卡 MAC（日志）
    std::string serial;     ///< 手机序列号（日志）
    bool unmanaged = false; ///< 有没有把网卡从 NetworkManager 手里摘出来
    bool pinned    = false; ///< 地址是这次钉的（false = 网卡本来就有）
};

enum class WiredPhase : int {
    Idle = 0,   ///< 还没开始
    Working,    ///< ① 在跑（USB / 配对 / 认证。锁屏时手机会弹配对框，得等用户点"信任"）
    Ready,      ///< ① 成功 —— 主线程现在该起 Receiver，然后 startPump()
    Failed,     ///< ① 失败，见 error()
    Pumping,    ///< ② 在跑（等 0x4300 / 已经把 0x4301 发出去了）
    Done,       ///< ② 结束（链路断了，或 stop() 了）
};

class WiredTask {
public:
    WiredTask();
    ~WiredTask() { stop(); }
    WiredTask(const WiredTask&) = delete;
    WiredTask& operator=(const WiredTask&) = delete;

    /// ① 起后台准备。非阻塞，立刻返回。上一次还在跑时返回 false（不排队、不打断了重来）。
    bool beginBringUp(const WiredTaskOptions& o, WiredLogFn log = nullptr);

    WiredPhase phase() const;
    /// 只在 phase() == Ready 之后有意义。
    const WiredBringUp& result() const;
    /// 失败原因 / ② 的收尾说明；没有则空串。
    std::string error() const;

    /// ② 等 0x4300 → 回 0x4301。**必须在 rx.start() 成功之后调**：
    /// 端口、deviceid、公钥都从 rx 上取（0x4301 里报的必须是 Receiver 自己认的那一套）。
    /// 这些值在调用线程上**按值快照**下来，线程里不会再碰 rx。
    bool startPump(const core::airplay::Receiver& rx, WiredLogFn log = nullptr);

    /// ② 结束没有（链路断开 / 收尾）。
    bool pumpDone() const;

    /// 收尾：置停止位 + join。pump 每 200ms 查一次停止位，很快回来；
    /// ① 还在跑的话要等它当前的阻塞调用超时（最长约一分钟），换来的是确定的析构顺序。
    void stop();

private:
    struct Impl;

    /// ② 要用的东西：全部在**调用线程**上按值快照好，线程里就不会再碰 rx。
    struct PumpParams {
        uint16_t    airPlayPort = 7000;
        /// 报给手机的那**一个**地址（= Receiver 绑的那个）。只报一个是有意的：
        /// 报多了手机会挑到我们没监听的那个地址上，connect 被拒。
        std::string address;
        std::string deviceId;
        std::string publicKeyHex;
    };

    /// 线程体。写成静态成员是为了能命名私有嵌套类型 Impl，同时保证不碰 this。
    static void runBringUp(std::shared_ptr<Impl> impl, WiredTaskOptions o, WiredLogFn log);
    static void runPump(std::shared_ptr<Impl> impl, PumpParams pp, WiredLogFn log);

    std::shared_ptr<Impl> impl_;
    std::thread           bringUpThread_;
    std::thread           pumpThread_;
};

}  // namespace allplay
