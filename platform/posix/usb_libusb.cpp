// ============================================================================
// platform/posix/usb_libusb.cpp —— 有线 CarPlay 的 USB 传输层（libusb）
// ============================================================================
// 整个有线链路里，只有这一段必须自己动手碰硬件。四步：
//   ① 找到 iPhone（Apple VID，排除键盘/集线器）
//   ② 让它换一套配置表：vendor 0xC0/0x52/index 4
//   ③ **自己选中** CarPlay 配置：libusb_set_configuration
//   ④ claim USBMUX 口，把它的两个 bulk 端点包成 core::IByteStream
//
// ② 和 ③ 分开是有血的教训的（真机实测）：vendor 0x52 只是让 iPhone 重枚举、多一套
// 配置表可选，**激活哪一个由主机决定**。Linux 内核按"哪个配置有驱动能绑"来挑，挑中的
// 是那个带 USB 网络共享的通用配置 —— 它照样能建出一块网卡，看起来像成功了，其实
// 媒体面根本不存在。所以 ③ 不能省。DiPlay 也是自己在 open 之后 setConfiguration 的。
//
// ③ 还会撞 LIBUSB_ERROR_BUSY。来源有三个，内核**不区分是谁占的**（只要当前配置下
// 任一接口被占就拒）：
//   · ipheth（内核驱动，绑在 Apple 以太网口上）→ 本文件自动 detach，切完配置内核会
//     自己重新 probe，不用还回去；
//   · usbmuxd（udev 每次重枚举都把它拉起来）→ 我们 detach 不了，只能让用户先停掉；
//   · gvfsd-gphoto2（GNOME 照片导入守护进程，占住 PTP 接口）→ 同上。
// 后两个的处置写在 kBusyHint 里，原样打给用户。
//
// 媒体面不在这里：切进 CarPlay 配置后，内核的 cdc_ncm 会自己绑上标准 CDC-NCM、把网卡
// 建出来，AirPlay 直接 bind 那块网卡的 IPv6 链路本地地址（见 usb.h 文件头）。
// 本文件只 claim USBMUX 一个接口，别的接口一概不碰。
#include "posix/usb.h"

#include "common/log.h"

#include <libusb-1.0/libusb.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace allplay::posix {

namespace {

constexpr const char* kTag = "usb";

constexpr uint16_t kAppleVendorId = 0x05ac;
// 同一台机器上会撞见的非 iPhone 苹果设备：列进来只会干扰判断
constexpr uint16_t kAppleKeyboardProduct = 0x024f;
constexpr uint16_t kAppleHubProduct      = 0x1006;

// 切配置表的 vendor 请求（与 DiPlay / LIVI 一致）
constexpr uint8_t  kCarPlayConfigRequest = 0x52;
constexpr uint16_t kCarPlayConfigIndex   = 0x0004;
constexpr uint8_t  kVendorDeviceIn       = 0xc0;
// 找不到 CarPlay 配置时，vendor 请求最多重发几次。给的是"人去解锁手机"的时间，
// 不是"等硬件慢慢反应"—— 真锁着屏的话再等也不会变，所以几次之后就报错并打印配置表。
constexpr int      kCarPlayConfigTries   = 3;

// 接口指纹
constexpr uint8_t kUsbMuxClass = 0xff, kUsbMuxSubclass = 0xfe, kUsbMuxProtocol = 0x02;
constexpr uint8_t kNcmControlClass = 0x02, kNcmControlSubclass = 0x0d;
constexpr uint8_t kNcmDataClass = 0x0a;
constexpr uint8_t kAppleEthClass = 0xff, kAppleEthSubclass = 0xfd, kAppleEthProtocol = 0x01;

// 读线程一次最多收多少。USBMUX 上不会有巨包，16K 够用又不浪费。
constexpr int kBulkInSize = 16 * 1024;
// 读线程的轮询间隔：既是空闲时的唤醒周期，也是 close() 之后的最坏退出延迟。
// 读线程不玩 async + cancel_transfer —— 那点收益不值这个复杂度。
constexpr int kBulkPollMs = 100;
// 写超时给得比读长得多：写不出去基本就是设备真没了。
constexpr int kBulkWriteTimeoutMs = 2000;

const char* kBusyHint =
    "当前配置下只要有任一接口被占着，内核就拒 —— 常驻的两个占用者要先让开：\n"
    "    sudo systemctl stop usbmuxd && sudo systemctl mask usbmuxd   # udev 每次重枚举都会把它拉起来\n"
    "    pkill -f gvfsd-gphoto2                                       # GNOME 照片导入，占住 iPhone 的 PTP 接口\n"
    "  到底是谁占的，内核日志里写着：journalctl -k -n 5";

// ---------------------------------------------------------------------------
// 描述符判断
// ---------------------------------------------------------------------------
bool isUsbMux(const libusb_interface_descriptor& d)
{
    return d.bInterfaceClass == kUsbMuxClass && d.bInterfaceSubClass == kUsbMuxSubclass &&
           d.bInterfaceProtocol == kUsbMuxProtocol;
}

bool isNcmControl(const libusb_interface_descriptor& d)
{
    return d.bInterfaceClass == kNcmControlClass && d.bInterfaceSubClass == kNcmControlSubclass;
}

bool isNcmData(const libusb_interface_descriptor& d)
{
    return d.bInterfaceClass == kNcmDataClass;
}

bool isAppleEthernet(const libusb_interface_descriptor& d)
{
    return d.bInterfaceClass == kAppleEthClass && d.bInterfaceSubClass == kAppleEthSubclass &&
           d.bInterfaceProtocol == kAppleEthProtocol;
}

/// CarPlay 配置 = USBMUX（ff/fe/02）+ **标准 CDC-NCM**（02/0d 控制口 + 0a 数据口）
/// + Apple 私有以太网（ff/fd/01）三件套齐活。
/// 别拿"配置号"认它：号随型号变（真 iPhone 是 6），只能按描述符找。
/// 三件套里只有 NCM 那两件是"网卡"的来源 —— 建网卡是内核 cdc_ncm 的活，本文件不插手。
struct CarPlayConfig {
    int index = -1;   ///< 描述符数组下标（libusb_get_config_descriptor 用）
    int value = -1;   ///< bConfigurationValue（set_configuration 用）
};

CarPlayConfig findCarPlayConfig(libusb_device* dev)
{
    CarPlayConfig found;
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(dev, &dd) != 0) return found;

    for (int i = 0; i < dd.bNumConfigurations; ++i) {
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_config_descriptor(dev, static_cast<uint8_t>(i), &cfg) != 0) continue;
        bool mux = false, ncm = false, eth = false;
        for (int k = 0; k < cfg->bNumInterfaces; ++k) {
            for (int a = 0; a < cfg->interface[k].num_altsetting; ++a) {
                const libusb_interface_descriptor& d = cfg->interface[k].altsetting[a];
                if (isUsbMux(d)) mux = true;
                if (isNcmControl(d)) ncm = true;
                if (isAppleEthernet(d)) eth = true;
            }
        }
        const int value = cfg->bConfigurationValue;
        libusb_free_config_descriptor(cfg);

        if (mux && eth && ncm) {
            found.index = i;
            found.value = value;
            break;
        }
    }
    return found;
}

/// 把当前的配置表原样打出来。**只在"找不到 CarPlay 配置"时才调用**
/// —— 这一条日志是"手机锁屏了"和"线/PTP 占位"的分水岭：
/// iPhone 锁屏时 iOS 只报 1 个配置（PTP），解锁并信任之后才把六个都报出来。
/// 没有它，两种完全不同的处境会共用同一句"换根数据线试试"，白折腾。
std::string describeConfigs(libusb_device* dev)
{
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(dev, &dd) != 0) return "（连设备描述符都读不到）";

    std::string out = "它现在报了 " + std::to_string(int(dd.bNumConfigurations)) + " 个配置：";
    for (int i = 0; i < dd.bNumConfigurations; ++i) {
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_config_descriptor(dev, static_cast<uint8_t>(i), &cfg) != 0) continue;
        out += "\n    [" + std::to_string(i) + "] value=" +
               std::to_string(int(cfg->bConfigurationValue)) + "，接口 " +
               std::to_string(int(cfg->bNumInterfaces)) + " 个：";
        for (int k = 0; k < cfg->bNumInterfaces; ++k) {
            // 只看主 altsetting：alt 0 已经能说明"这个接口是干什么的"，全打会刷屏
            const libusb_interface_descriptor& d = cfg->interface[k].altsetting[0];
            char b[32] = "";
            std::snprintf(b, sizeof b, " %02x/%02x/%02x", d.bInterfaceClass, d.bInterfaceSubClass,
                          d.bInterfaceProtocol);
            out += b;
        }
        libusb_free_config_descriptor(cfg);
    }
    return out;
}

// ---------------------------------------------------------------------------
// 设备枚举
// ---------------------------------------------------------------------------

/// 进程级共享的 libusb 上下文，**故意不 libusb_exit**：
/// 交出去的流（IByteStream）活到进程结束，退出上下文会把它的句柄一起作废。
libusb_context* usbContext()
{
    static libusb_context* c = []() -> libusb_context* {
        libusb_context* ctx = nullptr;
        if (libusb_init(&ctx) != 0) {
            LOG_ERROR("[%s] libusb_init 失败：装没装 libusb-1.0？", kTag);
            return nullptr;
        }
        libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
        return ctx;
    }();
    return c;
}

/// 设备列表的持有者：pick() 返回的裸指针只在这对象活着时有效。
struct DevList {
    libusb_device** list = nullptr;
    ssize_t n = 0;

    DevList() = default;
    DevList(const DevList&) = delete;
    DevList& operator=(const DevList&) = delete;
    ~DevList() { if (list) libusb_free_device_list(list, 1); }

    void refresh(libusb_context* ctx)
    {
        if (list) { libusb_free_device_list(list, 1); list = nullptr; }
        const ssize_t got = libusb_get_device_list(ctx, &list);
        if (got < 0) { list = nullptr; n = 0; return; }
        n = got;
    }

    /// 挑一台 iPhone。**不能**用"有没有 CarPlay 配置"来挑 —— 没切过配置的手机
    /// 描述符里还没有那张表，得先按 VID/PID 认人。
    libusb_device* pick(const WiredUsbOptions& opt) const
    {
        for (ssize_t i = 0; i < n; ++i) {
            libusb_device_descriptor dd{};
            if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
            if (dd.idVendor != opt.vendorId) continue;
            if (opt.productId >= 0) {
                if (dd.idProduct != opt.productId) continue;
            } else if (dd.idProduct == kAppleKeyboardProduct || dd.idProduct == kAppleHubProduct) {
                continue;
            }
            return list[i];
        }
        return nullptr;
    }
};

std::string openErrorHint(int rc)
{
    if (rc == LIBUSB_ERROR_ACCESS)
        return "打不开设备：权限不足。/dev/bus/usb/* 默认 root 独占 —— 用 sudo 跑，或加一条 udev 规则。";
    if (rc == LIBUSB_ERROR_NOT_FOUND)
        return "打不开设备：它刚刚消失了（多半正在重枚举），稍后重试。";
    return std::string("打不开设备：") + libusb_error_name(rc);
}

/// 内核驱动也得让开。USBDEVFS_SETCONFIGURATION 只要当前配置下任一接口被驱动绑着就
/// 返回 BUSY（内核日志原话：`interface 2 claimed by ipheth while 'usb_probe' sets
/// config #6`）。ipheth 会绑在 Apple 以太网口上，先 detach 掉；切完配置内核会自己
/// 重新 probe，不必还回去。
void detachKernelDrivers(libusb_device_handle* h)
{
    for (int i = 0; i < 32; ++i) {
        if (libusb_kernel_driver_active(h, i) != 1) continue;
        const int rc = libusb_detach_kernel_driver(h, i);
        LOG_INFO("[%s] 接口 %d 上有内核驱动 → detach：%s", kTag, i,
                 rc == 0 ? "成功" : libusb_error_name(rc));
    }
}

// ---------------------------------------------------------------------------
// USBMUX 的 bulk 管道 → core::IByteStream
// ---------------------------------------------------------------------------
// 与 BlueZ 那条流（platform/posix/bt_bluez.cpp 的 BlueZRfcommStream）语义一致：
// read 返回 >0 读到 / 0 超时 / <0 断开。但**所有权不同**：那条流是进程级、永不释放，
// 这条流归调用方，会话结束就析构 —— 所以这里老实 join 读线程、再由自己关句柄，
// 而不是 detach 了事。
class LibUsbBulkStream : public core::IByteStream {
public:
    LibUsbBulkStream(libusb_device_handle* h, uint8_t epIn, uint8_t epOut)
        : handle_(h), epIn_(epIn), epOut_(epOut)
    {
        reader_ = std::thread([this] { readerLoop(); });
    }

    ~LibUsbBulkStream() override
    {
        markClosed();
        if (reader_.joinable()) reader_.join();
        if (handle_) { libusb_close(handle_); handle_ = nullptr; }
    }

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                          [&] { return !queue_.empty() || closed_; }))
            return 0;                       // 超时
        if (queue_.empty()) return -1;      // 已断且没有残留
        const size_t take = std::min(n, queue_.size());
        for (size_t i = 0; i < take; ++i) { buf[i] = queue_.front(); queue_.pop_front(); }
        return int(take);
    }

    bool writeAll(const uint8_t* buf, size_t n) override
    {
        size_t off = 0;
        while (off < n) {
            int transferred = 0;
            const int rc = libusb_bulk_transfer(handle_, epOut_,
                                                const_cast<uint8_t*>(buf + off),
                                                int(n - off), &transferred, kBulkWriteTimeoutMs);
            if (rc != 0 || transferred <= 0) {
                LOG_WARN("[%s] bulk OUT 失败：%s", kTag, libusb_error_name(rc));
                markClosed();
                return false;
            }
            off += size_t(transferred);
        }
        return true;
    }

    void close() override { markClosed(); }

private:
    void markClosed()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    void readerLoop()
    {
        std::vector<uint8_t> buf(kBulkInSize);
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (closed_) break;
            }
            int transferred = 0;
            const int rc = libusb_bulk_transfer(handle_, epIn_, buf.data(), kBulkInSize,
                                                &transferred, kBulkPollMs);
            if (rc == LIBUSB_ERROR_TIMEOUT) continue;   // 空闲，正常
            if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO ||
                rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_OTHER) {
                LOG_WARN("[%s] bulk IN 断了：%s", kTag, libusb_error_name(rc));
                break;
            }
            if (rc != 0) continue;                      // 偶发错误，下一轮再试
            if (transferred <= 0) continue;
            {
                std::lock_guard<std::mutex> lk(mu_);
                queue_.insert(queue_.end(), buf.begin(), buf.begin() + transferred);
            }
            cv_.notify_all();
        }
        markClosed();
    }

    libusb_device_handle* handle_;
    const uint8_t epIn_;
    const uint8_t epOut_;

    std::thread             reader_;
    std::mutex              mu_;
    std::condition_variable cv_;
    std::deque<uint8_t>     queue_;
    bool                    closed_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// 对外：切进 CarPlay 配置 → 交出 USBMUX 管道
// ---------------------------------------------------------------------------
std::unique_ptr<core::IByteStream> openWiredCarPlay(const WiredUsbOptions& opt,
                                                    WiredUsbInfo* info,
                                                    std::string* err)
{
    auto fail = [&](const std::string& what) -> std::unique_ptr<core::IByteStream> {
        if (err) *err = what;
        LOG_ERROR("[%s] %s", kTag, what.c_str());
        return nullptr;
    };

    libusb_context* ctx = usbContext();
    if (!ctx) return fail("libusb 上下文建不起来");

    // ---- ① 找 iPhone ----
    DevList devs;
    devs.refresh(ctx);
    libusb_device* dev = devs.pick(opt);
    if (!dev) return fail("没找到 iPhone。插好了吗？用的是数据线吗（只能充电的线不行）？");

    libusb_device_descriptor dd{};
    libusb_get_device_descriptor(dev, &dd);

    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(dev, &handle);
    if (rc != 0) return fail(openErrorHint(rc));

    std::string serial;
    if (dd.iSerialNumber) {
        unsigned char sbuf[256] = {0};
        if (libusb_get_string_descriptor_ascii(handle, dd.iSerialNumber, sbuf,
                                               sizeof(sbuf) - 1) > 0)
            serial = reinterpret_cast<char*>(sbuf);
    }

    // ---- ②③ 把设备弄到"CarPlay 配置已选中、handle 还能用"这个状态 ----
    //
    // **重枚举是这台设备的常态**：vendor 请求必然触发一次；而 set_configuration 有时也会
    // 触发。重枚举之后旧 handle 立刻作废，后续第一个 bulk 传输就报 LIBUSB_ERROR_IO
    // （libusb 内部是 submiturb failed errno=2）—— 这个坑很隐蔽：前面每一步的返回值都是 0，
    // 只有真正开始收发时才炸。所以每一步之后一律"重新找一遍"，不去判断到底有没有重枚举
    // （判断不出来，也没必要）。
    auto reacquire = [&](const char* why) -> bool {
        if (handle != nullptr) {
            libusb_close(handle);
            handle = nullptr;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(opt.switchWaitMs);
        int lastOpen = 0;
        for (;;) {
            devs.refresh(ctx);
            dev = devs.pick(opt);
            if (dev != nullptr) {
                lastOpen = libusb_open(dev, &handle);
                if (lastOpen == 0) return true;
                handle = nullptr;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                fail(std::string(why) + "等不到 iPhone 回来（等了 " +
                     std::to_string(opt.switchWaitMs) + " ms）：" + openErrorHint(lastOpen));
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };

    // 最多绕几圈：重枚举要绕，配置表一时没上来也要绕。
    // cp 提到循环外面 —— 后面第 ④ 步还要拿它去读描述符。
    CarPlayConfig cp{};
    int           vendorTries = 0;
    for (int round = 0;; ++round) {
        if (round > 6) return fail("设备反复重新枚举，稳定不下来");

        // ---- ② 配置表里没有 CarPlay 配置 → 发 vendor 请求让它换一套 ----
        cp = findCarPlayConfig(dev);
        if (cp.index < 0) {
            if (vendorTries++ < kCarPlayConfigTries) {
                uint8_t resp = 0;
                rc = libusb_control_transfer(handle, kVendorDeviceIn, kCarPlayConfigRequest, 0,
                                             kCarPlayConfigIndex, &resp, sizeof(resp), 1000);
                LOG_INFO("[%s] 没有 CarPlay 配置，发 vendor 0x%02x / index %u：rc=%d（读到 0x%02x）"
                         "，第 %d/%d 次",
                         kTag, kCarPlayConfigRequest, kCarPlayConfigIndex, rc, resp, vendorTries,
                         kCarPlayConfigTries);
                if (rc != static_cast<int>(sizeof(resp)))
                    LOG_INFO("[%s]   注：这里 stall 不算失败，重枚举照样发生，以设备有没有变过为准",
                             kTag);
                if (!reacquire("切配置表之后")) return nullptr;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;  // 回到顶部重新看一遍配置表
            }

            // 试够了还是没有 —— 把配置表原样打出来。**只有到这一步，"手机锁屏"和
            // "线/口不对"才分得开**：只报 1 个 PTP 配置 = 手机锁着屏；报了好几个却凑不齐
            // 三件套 = 才是真的该换线。这两种处境以前共用一句"换根数据线"，白折腾人。
            libusb_device_descriptor dd2{};
            libusb_get_device_descriptor(dev, &dd2);
            const std::string how =
                dd2.bNumConfigurations <= 1
                    ? "它只报了 PTP 一个配置 —— 这正是 **iPhone 锁屏**时的样子（iOS 解锁前"
                      "不把 CarPlay 那套配置报出来）。解锁手机、让屏幕亮着，再重跑；"
                      "第一次还要在弹窗上点「信任此电脑」。"
                    : "配置表里没有「USBMUX + 标准 CDC-NCM + Apple 以太网」这一套 CarPlay"
                      "三件套 —— 换根原装数据线、或换一个 USB 口再试。";
            return fail("这台 iPhone 现在没给 CarPlay 配置。\n  " + how + "\n  " +
                        describeConfigs(dev));
        }

        // ---- ③ 自己选中 CarPlay 配置（不能省，见文件头） ----
        int active = -1;
        if (libusb_get_configuration(handle, &active) != 0 || active != cp.value) {
            detachKernelDrivers(handle);
            rc = libusb_set_configuration(handle, cp.value);
            if (rc != 0) {
                std::string msg = "set_configuration(" + std::to_string(cp.value) + ") 失败：" +
                                  libusb_error_name(rc);
                if (rc == LIBUSB_ERROR_BUSY) msg += "\n  " + std::string(kBusyHint);
                libusb_close(handle);
                return fail(msg);
            }
            // 重新拿 handle；下一圈回来重新读一次配置，确认真的生效了（别只信返回值）
            if (!reacquire("选中 CarPlay 配置之后")) return nullptr;
            continue;
        }

        LOG_INFO("[%s] 已经在 CarPlay 配置 %d 上", kTag, cp.value);
        break;
    }

    // ---- ④ 认出各接口，claim USBMUX ----
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_config_descriptor(dev, static_cast<uint8_t>(cp.index), &cfg) != 0)
        return fail("读不到 CarPlay 配置的描述符");
    struct CfgGuard {
        libusb_config_descriptor* c;
        ~CfgGuard() { libusb_free_config_descriptor(c); }
    } cfgGuard{cfg};

    const libusb_interface* mux = nullptr;
    int ncmControl = -1;
    int ncmData = -1;
    for (int i = 0; i < cfg->bNumInterfaces; ++i) {
        for (int a = 0; a < cfg->interface[i].num_altsetting; ++a) {
            const libusb_interface_descriptor& d = cfg->interface[i].altsetting[a];
            if (isUsbMux(d) && !mux) mux = &cfg->interface[i];
            if (isNcmControl(d) && ncmControl < 0) ncmControl = d.bInterfaceNumber;
            if (isNcmData(d) && ncmData < 0) ncmData = d.bInterfaceNumber;
        }
    }
    if (!mux) {
        libusb_close(handle);
        return fail("CarPlay 配置里找不到 USBMUX 接口（ff/fe/02）");
    }

    // 端点不写死：从描述符里找成对的 bulk。实测这台是 out=0x04 / in=0x85，
    // 但那是"这一台"的取值，不是协议。
    uint8_t epIn = 0;
    uint8_t epOut = 0;
    for (int a = 0; a < mux->num_altsetting; ++a) {
        const libusb_interface_descriptor& d = mux->altsetting[a];
        for (int e = 0; e < d.bNumEndpoints; ++e) {
            const libusb_endpoint_descriptor& ep = d.endpoint[e];
            if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                if (!epIn) epIn = ep.bEndpointAddress;
            } else if (!epOut) {
                epOut = ep.bEndpointAddress;
            }
        }
    }
    const uint8_t muxNumber = mux->altsetting[0].bInterfaceNumber;
    if (!epIn || !epOut) {
        libusb_close(handle);
        return fail("USBMUX 接口上没有成对的 bulk 端点");
    }

    // usbmuxd 之类如果还占着，这里会 BUSY —— 和 set_configuration 是同一类占用者
    if (libusb_kernel_driver_active(handle, muxNumber) == 1)
        libusb_detach_kernel_driver(handle, muxNumber);
    rc = libusb_claim_interface(handle, muxNumber);
    if (rc != 0) {
        std::string msg = std::string("claim USBMUX 接口失败：") + libusb_error_name(rc);
        if (rc == LIBUSB_ERROR_BUSY) msg += "\n  多半是 usbmuxd 占着。\n  " + std::string(kBusyHint);
        libusb_close(handle);
        return fail(msg);
    }

    if (info) {
        *info = WiredUsbInfo{};
        info->vendorId = dd.idVendor;
        info->productId = dd.idProduct;
        info->serial = serial;
        info->bus = libusb_get_bus_number(dev);
        info->address = libusb_get_device_address(dev);
        info->configValue = cp.value;
        info->usbMuxInterface = muxNumber;
        info->bulkIn = epIn;
        info->bulkOut = epOut;
        info->ncmControlInterface = static_cast<uint8_t>(std::max(ncmControl, 0));
        info->ncmDataInterface = static_cast<uint8_t>(std::max(ncmData, 0));
    }

    // 这条日志直接点名"网卡待会儿由谁来建"（内核 cdc_ncm）—— 出问题时第一眼看的就是它：
    // 这里说 cdc_ncm 而 `ip link` 里没网卡 = CDC 那套没绑上。
    LOG_INFO("[%s] USBMUX 就绪：%u:%u 序列号=%s 配置=%d 接口=%u 端点 out=0x%02x in=0x%02x"
             "（NCM 控制口 %d / 数据口 %d —— 网卡那摊交给内核 cdc_ncm）",
             kTag, libusb_get_bus_number(dev), libusb_get_device_address(dev),
             serial.empty() ? "?" : serial.c_str(), cp.value, muxNumber, epOut, epIn,
             ncmControl, ncmData);

    return std::unique_ptr<core::IByteStream>(new LibUsbBulkStream(handle, epIn, epOut));
}

}  // namespace allplay::posix
