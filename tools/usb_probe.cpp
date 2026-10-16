// ============================================================================
// tools/usb_probe.cpp —— 有线 CarPlay 的 USB 侧探针（整条路的第一道验证）
// ============================================================================
// 有线链路的第一道门是"切配置"：iPhone 插上时是个普通配置，**只有把它切进
// CarPlay 配置**，才会有 USBMUX(ff/fe/02) + CDC-NCM(02/0d) + Apple 以太网(ff/fd/01)
// 这几个功能 —— iAP2 走 USBMUX 口，AirPlay 走 NCM 变成的那块网卡。
//
// 切换方式（参照 DiPlay / LIVI 的行为）：对设备发 vendor 请求
//   bmRequestType=0xC0  bRequest=0x52  wValue=0  wIndex=4   读 1 字节
// iPhone 收到后会**重新枚举**，新配置里上述接口才出现。配置号随 iPhone 型号变，
// 所以不能写死，只能按描述符找。
//
// **踩过的坑（真机实测）**：光发 vendor 0x52 不够。它只是让 iPhone 重枚举换一套
// 配置表，但**重枚举之后由主机挑激活哪一个** —— Linux 内核按"哪个配置有驱动能绑"
// 来挑，结果挑中的是那个通用配置（PTP + USBMUX + Apple 以太网，带 USB 网络共享），
// 而不是 CarPlay 配置。表现极具迷惑性：`ip -br link` 里**确实多出一块网卡**，
// 但那是 ipheth 给共享网络建的，媒体面根本不存在。
// 所以必须自己 `libusb_set_configuration()` 显式选中 CarPlay 那个配置值
// （DiPlay 也是在 open 之后自己 setConfiguration 的）。
//
// 这个工具只做三件事：枚举、切配置 + 选配置、再看一遍描述符。它**不碰 iAP2、不碰 NCM 数据面**。
// 价值是把"能不能"从推测变成实测，具体回答三个问题：
//   ① 切之前 / 切之后，CarPlay 配置到底在不在？（不在 → 有线这条路走不通）
//   ② 显式选中它之后，Linux 内核认没认出 NCM 网卡？（跑完顺手看 `ip -br link` 与
//      `journalctl -k`；认出来了，AirPlay 那段就能白拿内核的 TCP/IP，不必自己写 NCM 数据面）
//   ③ 这块网卡上能不能拿到 IPv6 链路本地地址（AirPlay 要 bind 到它）
//
// 用法：
//   usb_probe --list                   只列设备（Apple 的会展开配置/接口/端点）
//   usb_probe --carplay [--wait 10]    切配置 → 显式选中 CarPlay 配置（最常用）
//   usb_probe --switch [--wait 10]     只切配置 → 等重枚举 → 再列一遍（不选）
//   usb_probe --setcfg 6               只显式选某个配置，不发 vendor 请求
//   usb_probe --dump 1:5               只 dump 指定 bus:addr
// 选项：
//   --vid 05ac        厂商号过滤（默认只认 Apple）
//   --pid 12a8        产品号过滤（默认不限，键盘/集线器会被自动排除）
//   --any-apple       连键盘/集线器一起列（默认排除，免干扰）
//
// 权限：Linux 下 /dev/bus/usb/* 默认 root 独占（755）。非 root 打开会
// LIBUSB_ERROR_ACCESS —— 用 sudo 跑，或自己写一条 udev 规则。本工具会直接提示。
//
// 另一个拦路的：**usbmuxd**。它会被 udev 在每次重枚举时自动拉起，一上来就占用
// USBMUX 接口（sysfs 里显示 driver=usbfs），此时 set_configuration 会返回 BUSY。
// 跑之前 `sudo systemctl stop usbmuxd`；要一劳永逸就 `systemctl mask usbmuxd`。
//
// 一个坑：切配置会重枚举，**老的 handle 立刻失效**，设备路径也会变
// （bus/addr 不同）。所以切换后必须重新枚举、按 VID/PID 重新找，不能复用旧 handle。
#include <libusb-1.0/libusb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 常量：Apple 的接口指纹（与 DiPlay 的 IphoneCarPlayConfiguration 对齐）
// ---------------------------------------------------------------------------
constexpr uint16_t kAppleVendorId = 0x05ac;

// USBMUX：iAP2 的载体。注意是 ff/fe/02，不是常见的 iAP 接口。
constexpr uint8_t kUsbMuxClass = 0xff;
constexpr uint8_t kUsbMuxSubclass = 0xfe;
constexpr uint8_t kUsbMuxProtocol = 0x02;
// CDC-NCM 控制口：内核 cdc_ncm 认的就是这个。
constexpr uint8_t kNcmControlClass = 0x02;
constexpr uint8_t kNcmControlSubclass = 0x0d;
// Apple 专有以太网功能：CarPlay 配置的第三块拼图。
constexpr uint8_t kAppleEthernetClass = 0xff;
constexpr uint8_t kAppleEthernetSubclass = 0xfd;
constexpr uint8_t kAppleEthernetProtocol = 0x01;

// 切配置的 vendor 请求（DiPlay 的取值，来源 LIVI 的固定 commit）
constexpr uint8_t kCarPlayConfigRequest = 0x52;
constexpr uint16_t kCarPlayConfigIndex = 0x0004;
constexpr uint8_t kVendorDeviceIn = 0xc0;

// 这些 Apple 设备不是 iPhone，列出来只会干扰判断（用户这台是苹果键盘 + 键盘上的 Hub）
constexpr uint16_t kAppleKeyboardProduct = 0x024f;
constexpr uint16_t kAppleHubProduct = 0x1006;

struct Options {
    bool        list = false;      // --list
    bool        doSwitch = false;  // --switch
    bool        doCarPlay = false; // --carplay
    int         setCfg = -1;       // --setcfg N
    bool        anyApple = false;  // --any-apple
    std::string dump;              // --dump bus:addr
    int         waitSeconds = 10;  // --wait：等重枚举的秒数
    uint16_t    vid = kAppleVendorId;
    int         pid = -1;          // -1 = 不限
};

void printUsage() {
    std::fprintf(stderr,
        "用法：\n"
        "  usb_probe --carplay [--wait 10]    切配置 → 显式选中 CarPlay 配置 → 报结果（最常用）\n"
        "  usb_probe --switch  [--wait 10]    只发 vendor 0x52 切配置 → 等重枚举 → 再列\n"
        "  usb_probe --setcfg 6               只显式选某个配置（不发 vendor 请求）\n"
        "  usb_probe --list                   列出 USB 设备（Apple 的展开配置/接口/端点）\n"
        "  usb_probe --dump 1:5               只 dump 指定 bus:addr\n"
        "选项：\n"
        "  --vid 05ac     厂商号过滤（默认 Apple）\n"
        "  --pid 12a8     产品号过滤（默认不限）\n"
        "  --any-apple    连键盘/集线器一起列\n"
        "跑之前先 `sudo systemctl stop usbmuxd`，否则它占着 USBMUX 接口，setcfg 会 BUSY。\n");
}

// ---------------------------------------------------------------------------
// 描述符打印：格式对齐 DiPlay 的 describe()，方便两边对着看
//   "接口号/备用设置:类.子类.协议x端点数"
// ---------------------------------------------------------------------------
struct ConfigSummary {
    int     id = -1;
    bool    hasUsbMux = false;
    bool    hasCdcNcm = false;
    bool    hasAppleEthernet = false;
    uint8_t usbMuxInterface = 0;
    /// CarPlay 配置 = USBMUX + 标准 CDC-NCM + Apple 私有以太网，三件套齐活。
    /// 判据与 platform/posix/usb_libusb.cpp 的 findCarPlayConfig 保持同一套。
    bool    carPlay() const { return hasUsbMux && hasCdcNcm && hasAppleEthernet; }
};

std::string interfaceLine(const libusb_interface_descriptor& d) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%u/%u:%02x.%02x.%02xx%u",
                  d.bInterfaceNumber, d.bAlternateSetting,
                  d.bInterfaceClass, d.bInterfaceSubClass, d.bInterfaceProtocol,
                  d.bNumEndpoints);
    return buf;
}

std::string endpointLine(const libusb_endpoint_descriptor& e) {
    const char* dir = (e.bEndpointAddress & LIBUSB_ENDPOINT_IN) ? "IN " : "OUT";
    const char* type = "?";
    switch (e.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) {
        case LIBUSB_TRANSFER_TYPE_CONTROL:     type = "control"; break;
        case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS: type = "isoc"; break;
        case LIBUSB_TRANSFER_TYPE_BULK:        type = "bulk"; break;
        case LIBUSB_TRANSFER_TYPE_INTERRUPT:   type = "int"; break;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "      ep 0x%02x %s %s maxPacket=%u",
                  e.bEndpointAddress, dir, type, e.wMaxPacketSize);
    return buf;
}

ConfigSummary summarize(const libusb_config_descriptor& cfg) {
    ConfigSummary s;
    s.id = cfg.bConfigurationValue;
    for (int i = 0; i < cfg.bNumInterfaces; ++i) {
        for (int a = 0; a < cfg.interface[i].num_altsetting; ++a) {
            const libusb_interface_descriptor& d = cfg.interface[i].altsetting[a];
            if (d.bInterfaceClass == kUsbMuxClass && d.bInterfaceSubClass == kUsbMuxSubclass &&
                d.bInterfaceProtocol == kUsbMuxProtocol) {
                s.hasUsbMux = true;
                s.usbMuxInterface = d.bInterfaceNumber;
            }
            if (d.bInterfaceClass == kNcmControlClass && d.bInterfaceSubClass == kNcmControlSubclass) {
                s.hasCdcNcm = true;
            }
            if (d.bInterfaceClass == kAppleEthernetClass &&
                d.bInterfaceSubClass == kAppleEthernetSubclass &&
                d.bInterfaceProtocol == kAppleEthernetProtocol) {
                s.hasAppleEthernet = true;
            }
        }
    }
    return s;
}

void printConfig(const libusb_config_descriptor& cfg) {
    const ConfigSummary s = summarize(cfg);
    std::printf("    配置 %d：%d 个接口  [usbmux=%s cdc-ncm=%s apple-eth=%s]%s\n",
                cfg.bConfigurationValue, cfg.bNumInterfaces,
                s.hasUsbMux ? "有" : "无", s.hasCdcNcm ? "有" : "无",
                s.hasAppleEthernet ? "有" : "无",
                s.carPlay() ? "   <<< CarPlay 配置（内核 cdc_ncm 建网卡）" : "");
    for (int i = 0; i < cfg.bNumInterfaces; ++i) {
        for (int a = 0; a < cfg.interface[i].num_altsetting; ++a) {
            const libusb_interface_descriptor& d = cfg.interface[i].altsetting[a];
            std::printf("      %s\n", interfaceLine(d).c_str());
            for (int e = 0; e < d.bNumEndpoints; ++e) {
                std::printf("%s\n", endpointLine(d.endpoint[e]).c_str());
            }
        }
    }
}

// 打开设备读**全部**配置。iPhone 会同时声明好几个配置，只有一个是 CarPlay。
bool describeDevice(libusb_device* dev, bool verbose) {
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(dev, &dd) != 0) return false;

    const uint8_t bus = libusb_get_bus_number(dev);
    const uint8_t addr = libusb_get_device_address(dev);
    std::printf("  %u:%u  %04x:%04x  %d 个配置\n", bus, addr, dd.idVendor, dd.idProduct,
                dd.bNumConfigurations);
    if (!verbose) return true;

    // 设备描述符里能直接读到全部配置（不必 open），读描述符不需要权限。
    bool anyCarPlay = false;
    for (int c = 0; c < dd.bNumConfigurations; ++c) {
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_config_descriptor(dev, static_cast<uint8_t>(c), &cfg) != 0) continue;
        printConfig(*cfg);
        if (summarize(*cfg).carPlay()) anyCarPlay = true;
        libusb_free_config_descriptor(cfg);
    }
    if (dd.bNumConfigurations == 0) {
        std::printf("    （读不到配置描述符）\n");
    } else if (!anyCarPlay) {
        std::printf("    → 没有 CarPlay 配置（USBMUX+NCM+Apple以太网 三件套不全）\n");
    }
    return true;
}

bool isCandidate(const libusb_device_descriptor& dd, const Options& opt) {
    if (opt.vid != 0 && dd.idVendor != opt.vid) return false;
    if (opt.pid >= 0 && dd.idProduct != static_cast<uint16_t>(opt.pid)) return false;
    if (!opt.anyApple && dd.idVendor == kAppleVendorId &&
        (dd.idProduct == kAppleKeyboardProduct || dd.idProduct == kAppleHubProduct)) {
        return false;
    }
    return true;
}

std::vector<libusb_device*> collect(libusb_context* ctx, const Options& opt) {
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    std::vector<libusb_device*> out;
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        if (!isCandidate(dd, opt)) continue;
        out.push_back(libusb_ref_device(list[i]));  // 自己持引用，list 可以释放
    }
    libusb_free_device_list(list, 1);
    return out;
}

void releaseAll(std::vector<libusb_device*>& devs) {
    for (libusb_device* d : devs) libusb_unref_device(d);
    devs.clear();
}

// "1:5" → bus=1 addr=5
bool parseBusAddr(const std::string& s, uint8_t* bus, uint8_t* addr) {
    const size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    *bus = static_cast<uint8_t>(std::atoi(s.substr(0, colon).c_str()));
    *addr = static_cast<uint8_t>(std::atoi(s.substr(colon + 1).c_str()));
    return true;
}

std::string accessHint(int rc) {
    if (rc == LIBUSB_ERROR_ACCESS) {
        return "  ← 权限不足：/dev/bus/usb 默认 root 独占，换 sudo 跑，或加一条 udev 规则";
    }
    if (rc == LIBUSB_ERROR_NOT_FOUND) {
        return "  ← 设备已消失（多半是刚刚重枚举了）";
    }
    return "";
}

// ---------------------------------------------------------------------------
// 切配置：发 vendor 请求 → 等重枚举
// ---------------------------------------------------------------------------

// 返回 CarPlay 配置的 bConfigurationValue（USBMUX+NCM+Apple以太网 三件套齐全的那个）。
// 型号之间配置号不同，所以只能按描述符找，不能写死；没有这样的配置返回 -1。
int findCarPlayConfig(libusb_device* dev) {
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(dev, &dd) != 0) return -1;
    for (int c = 0; c < dd.bNumConfigurations; ++c) {
        libusb_config_descriptor* cfg = nullptr;
        if (libusb_get_config_descriptor(dev, static_cast<uint8_t>(c), &cfg) != 0) continue;
        const bool ok = summarize(*cfg).carPlay();
        const int value = cfg->bConfigurationValue;
        libusb_free_config_descriptor(cfg);
        if (ok) return value;
    }
    return -1;
}

void describeAll(libusb_context* ctx, const Options& opt, const char* title) {
    std::printf("%s\n", title);
    std::vector<libusb_device*> devs = collect(ctx, opt);
    if (devs.empty()) std::printf("  （一个都没有 —— 设备还没回来，稍等一下再跑）\n");
    for (libusb_device* d : devs) describeDevice(d, true);
    releaseAll(devs);
}

// 发 vendor 0x52 让 iPhone 重枚举。返回 true = 确实观察到重枚举。
// **它只负责让手机换一套配置表，不负责选中哪一个** —— 见 setActiveConfig 上面的注释。
bool requestCarPlayConfig(libusb_context* ctx, const Options& opt) {
    std::vector<libusb_device*> devs = collect(ctx, opt);
    if (devs.empty()) {
        std::printf("没找到匹配的设备（vid=%04x pid=%s）。iPhone 插好了吗？数据线是数据线吗？\n",
                    opt.vid, opt.pid < 0 ? "任意" : "指定");
        return false;
    }

    libusb_device* target = devs.front();
    libusb_device_descriptor dd{};
    libusb_get_device_descriptor(target, &dd);
    const uint8_t bus = libusb_get_bus_number(target);
    const uint8_t addr = libusb_get_device_address(target);
    std::printf("目标：%u:%u  %04x:%04x\n", bus, addr, dd.idVendor, dd.idProduct);

    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(target, &handle);
    if (rc != 0) {
        std::printf("打不开设备：%s%s\n", libusb_error_name(rc), accessHint(rc).c_str());
        releaseAll(devs);
        return false;
    }
    releaseAll(devs);  // handle 自己持引用，device 列表可以放了

    uint8_t response = 0;
    rc = libusb_control_transfer(handle, kVendorDeviceIn, kCarPlayConfigRequest, 0,
                                 kCarPlayConfigIndex, &response, sizeof(response), 1000);
    std::printf("\n发 vendor 0x%02x / index %u：rc=%d", kCarPlayConfigRequest, kCarPlayConfigIndex, rc);
    if (rc == static_cast<int>(sizeof(response))) {
        std::printf("（读到 0x%02x）\n", response);
    } else {
        std::printf("%s\n", accessHint(rc).c_str());
        std::printf("  注：有些固件这里会 stall（LIBUSB_ERROR_IO/PIPE），但**重枚举照样发生**，"
                    "以设备有没有变过为准。\n");
    }
    libusb_close(handle);

    // 等重枚举：老的 (bus,addr) 消失 + 出现同 VID/PID 的新设备，两者都见到才算切成功。
    std::printf("\n等重枚举（最多 %d 秒）...\n", opt.waitSeconds);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(opt.waitSeconds);
    bool oldGone = false;
    bool freshSeen = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<libusb_device*> now = collect(ctx, opt);
        bool sawOld = false;
        bool sawNew = false;
        for (libusb_device* d : now) {
            if (libusb_get_bus_number(d) == bus && libusb_get_device_address(d) == addr) sawOld = true;
            else sawNew = true;
        }
        oldGone = oldGone || !sawOld;
        freshSeen = freshSeen || sawNew;
        releaseAll(now);
        if (oldGone && freshSeen) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return oldGone && freshSeen;
}

// 显式选中一个配置值。
// 为什么非做不可：vendor 0x52 只是让 iPhone 重枚举，**重枚举后激活哪个配置是主机说了算**。
// Linux 内核挑的是"哪个配置的接口有驱动能绑"，于是挑中带 USB 网络共享的那个通用配置，
// CarPlay 配置压根没被激活 —— 内核只会用 ipheth 建出一块共享网络的网卡来骗你。
// DiPlay 同样是自己 setConfiguration 的。
int setActiveConfig(libusb_context* ctx, const Options& opt, int value) {
    std::vector<libusb_device*> devs = collect(ctx, opt);
    if (devs.empty()) {
        std::printf("没找到设备，没法选配置\n");
        return 1;
    }
    libusb_device* target = devs.front();
    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(target, &handle);
    if (rc != 0) {
        std::printf("打不开设备：%s%s\n", libusb_error_name(rc), accessHint(rc).c_str());
        releaseAll(devs);
        return 1;
    }
    releaseAll(devs);

    // 先让内核驱动让开。USBDEVFS_SETCONFIGURATION 只要**当前配置下任何一个接口**被占着
    // 就返回 BUSY —— 不区分占用者是谁。实测这台机器上两个占用者：
    //   · ipheth（内核驱动，绑在 Apple 以太网口上）→ 本函数 detach 掉；它只是 USB 网络共享，
    //     切完配置内核会自己重新 probe，不用管。
    //   · usbmuxd / gvfsd-gphoto2（用户态进程）→ 我们 detach 不了，只能让用户先停掉；
    //     内核日志会写明是谁。这就是下面那段提示的由来。
    int detached[32];
    int nDetached = 0;
    for (int i = 0; i < 32 && nDetached < 32; ++i) {
        if (libusb_kernel_driver_active(handle, i) != 1) continue;
        const int d = libusb_detach_kernel_driver(handle, i);
        std::printf("  接口 %d 上有内核驱动 → detach：%s\n", i, d == 0 ? "成功" : libusb_error_name(d));
        if (d == 0) detached[nDetached++] = i;
    }

    rc = libusb_set_configuration(handle, value);
    if (rc != 0) {
        std::printf("set_configuration(%d) 失败：%s\n", value, libusb_error_name(rc));
        if (rc == LIBUSB_ERROR_BUSY) {
            std::printf("  ← 当前配置下有接口被占着。内核驱动本工具已经 detach 过了，"
                        "那剩下就是用户态进程：\n"
                        "     sudo systemctl stop usbmuxd      # 每次重枚举 udev 都会把它拉起来，"
                        "要一劳永逸就 systemctl mask usbmuxd\n"
                        "     pkill -f gvfsd-gphoto2           # GNOME 的照片导入守护进程，"
                        "会占住 iPhone 的 PTP 接口\n"
                        "  到底是谁占的，内核日志里写着：journalctl -k -n 5\n");
        }
        for (int i = 0; i < nDetached; ++i) libusb_attach_kernel_driver(handle, detached[i]);
        libusb_close(handle);
        return 1;
    }
    int active = -1;
    libusb_get_configuration(handle, &active);  // 回读确认，别只信返回值
    std::printf("set_configuration(%d) 成功，当前激活配置 = %d\n", value, active);
    libusb_close(handle);
    return 0;
}

int doSwitch(libusb_context* ctx, const Options& opt) {
    describeAll(ctx, opt, "切之前：");
    const bool ok = requestCarPlayConfig(ctx, opt);
    std::printf("  %s\n", ok ? "设备重枚举了 ✓" : "没观察到重枚举（可能没切成功）");
    describeAll(ctx, opt, "\n切之后：");

    std::printf("\n注意：重枚举只说明手机换了一套配置表，**当前激活的是哪一个还得自己选**（用 --carplay）。\n"
                "接着看内核认没认出 NCM 网卡：\n"
                "  ip -br link; ip -br addr; lsusb -t\n"
                "  journalctl -k -n 30 | grep -E 'ipheth|cdc_ncm|usb 1-'\n");
    return ok ? 0 : 1;
}

// --carplay：把"能不能进 CarPlay 配置"一次问到底
int doCarPlay(libusb_context* ctx, const Options& opt) {
    describeAll(ctx, opt, "切之前：");
    const bool ok = requestCarPlayConfig(ctx, opt);
    std::printf("  %s\n", ok ? "设备重枚举了 ✓" : "没观察到重枚举（可能没切成功）");

    std::vector<libusb_device*> devs = collect(ctx, opt);
    if (devs.empty()) {
        std::printf("\n设备没回来，稍等再跑一次。\n");
        return 1;
    }
    const int cp = findCarPlayConfig(devs.front());
    releaseAll(devs);
    if (cp < 0) {
        std::printf("\n这台 iPhone 没有 CarPlay 配置（USBMUX + CDC-NCM + Apple 以太网 三件套）\n"
                    "→ 有线这条路走不通。\n");
        describeAll(ctx, opt, "\n实际描述符：");
        return 1;
    }
    std::printf("\nCarPlay 配置 = %d（按描述符找的，别写死）\n", cp);

    if (setActiveConfig(ctx, opt, cp) != 0) return 1;

    describeAll(ctx, opt, "\n选中之后：");
    std::printf("\n现在该有两类网卡：ipheth 那块是共享网络的（还在，无害），"
                "cdc_ncm 给 CarPlay 建的那块才是媒体面：\n"
                "  ip -br link; lsusb -t; journalctl -k -n 30\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 后面要跟一个值\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--list"))            opt.list = true;
        else if (!std::strcmp(a, "--switch"))     opt.doSwitch = true;
        else if (!std::strcmp(a, "--carplay"))    opt.doCarPlay = true;
        else if (!std::strcmp(a, "--setcfg"))     opt.setCfg = std::atoi(next("--setcfg"));
        else if (!std::strcmp(a, "--any-apple"))  opt.anyApple = true;
        else if (!std::strcmp(a, "--dump"))       opt.dump = next("--dump");
        else if (!std::strcmp(a, "--wait"))       opt.waitSeconds = std::atoi(next("--wait"));
        else if (!std::strcmp(a, "--vid"))        opt.vid = static_cast<uint16_t>(std::strtoul(next("--vid"), nullptr, 16));
        else if (!std::strcmp(a, "--pid"))        opt.pid = static_cast<int>(std::strtoul(next("--pid"), nullptr, 16));
        else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) { printUsage(); return 0; }
        else {
            std::fprintf(stderr, "未知参数：%s\n", a);
            printUsage();
            return 2;
        }
    }
    if (!opt.list && !opt.doSwitch && !opt.doCarPlay && opt.setCfg < 0 && opt.dump.empty()) {
        printUsage();
        return 2;
    }

    libusb_context* ctx = nullptr;
    const int rc = libusb_init(&ctx);
    if (rc != 0) {
        std::fprintf(stderr, "libusb_init 失败：%s\n", libusb_error_name(rc));
        return 1;
    }
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);

    int ret = 0;
    if (opt.doCarPlay) {
        ret = doCarPlay(ctx, opt);
    } else if (opt.doSwitch) {
        ret = doSwitch(ctx, opt);
    } else if (opt.setCfg >= 0) {
        ret = setActiveConfig(ctx, opt, opt.setCfg);
    } else {
        std::vector<libusb_device*> devs = collect(ctx, opt);
        if (!opt.dump.empty()) {
            uint8_t bus = 0;
            uint8_t addr = 0;
            if (!parseBusAddr(opt.dump, &bus, &addr)) {
                std::fprintf(stderr, "--dump 要写成 bus:addr，例如 1:5\n");
                releaseAll(devs);
                libusb_exit(ctx);
                return 2;
            }
            bool found = false;
            for (libusb_device* d : devs) {
                if (libusb_get_bus_number(d) == bus && libusb_get_device_address(d) == addr) {
                    describeDevice(d, true);
                    found = true;
                }
            }
            if (!found) std::printf("没有 %u:%u 这个设备（注意切配置后地址会变）\n", bus, addr);
        } else {
            std::printf("匹配到 %zu 个设备：\n", devs.size());
            for (libusb_device* d : devs) describeDevice(d, true);
        }
        releaseAll(devs);
    }

    libusb_exit(ctx);
    return ret;
}
