// ============================================================================
// platform/posix/net_posix.cpp —— TCP 监听/连接 + 网卡信息（Linux/macOS 共用）
// ============================================================================
// 实现 core::ITcpListener / core::ITcpStream / core::INetworkInfo。
// 读超时用 poll() 按调用给；accept 用 poll() 轮询，stop() 关闭监听 fd 即会退出。
#include "posix/posix.h"

#include "common/log.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>          // pclose 的退出码（有线 NCM bring-up 走 shell）
#include <unistd.h>

#if defined(__APPLE__)
#  include <net/if_dl.h>       // sockaddr_dl / LLADDR
#else
#  include <linux/if_packet.h> // sockaddr_ll（AF_PACKET）
#  include <linux/sockios.h>   // SIOCGIFHWADDR
#  include <sys/ioctl.h>
#endif

#include <algorithm>           // std::swap
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(__APPLE__)
#  include <dirent.h>          // /sys/class/net 枚举（有线 CarPlay 的 NCM 网卡）
#  include <cstdlib>
#endif

namespace allplay::posix {

namespace {

// macOS 无 MSG_NOSIGNAL（往断开的 socket 写会 SIGPIPE）→ 用 SO_NOSIGPIPE。
#if !defined(MSG_NOSIGNAL)
#  define MSG_NOSIGNAL 0
#endif

void setNoSigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

// ---------------------------------------------------------------------------
// TCP 流
// ---------------------------------------------------------------------------
class PosixTcpStream : public core::ITcpStream {
public:
    explicit PosixTcpStream(int fd) : fd_(fd) {}
    ~PosixTcpStream() override { close(); }
    PosixTcpStream(const PosixTcpStream&) = delete;

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        if (fd_ < 0) return -1;
        struct pollfd pfd{fd_, POLLIN, 0};
        int r = ::poll(&pfd, 1, timeoutMs < 0 ? -1 : timeoutMs);
        if (r == 0) return 0;                       // 超时
        if (r < 0) {
            if (errno == EINTR) return read(buf, n, timeoutMs);
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
        ssize_t got = ::recv(fd_, buf, n, 0);
        if (got > 0) return int(got);
        if (got == 0) return -1;                    // 对端关闭
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }

    bool writeAll(const uint8_t* buf, size_t n) override
    {
        if (fd_ < 0) return false;
        size_t off = 0;
        while (off < n) {
            ssize_t w = ::send(fd_, buf + off, n - off, MSG_NOSIGNAL);
            if (w <= 0) return false;
            off += size_t(w);
        }
        return true;
    }

    void close() override
    {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

private:
    int fd_;
};

// ---------------------------------------------------------------------------
// TCP 监听
// ---------------------------------------------------------------------------
class PosixTcpListener : public core::ITcpListener {
public:
    ~PosixTcpListener() override { stop(); }

    bool start(const std::string& bindAddr, uint16_t port, std::string* err) override
    {
        // IPv6 双栈监听：一个监听器同时收 IPv4 与 IPv6（iPhone 的 CarPlay 常走 IPv6）
        int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0) {
            if (err) *err = "socket 失败：" + std::string(std::strerror(errno));
            return false;
        }
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        int zero = 0;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);   // 双栈：也收 IPv4

        struct sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(port);
        addr.sin6_addr = in6addr_any;
        if (!bindAddr.empty()) {
            // "地址%网卡" 的写法要在这里拆开：**链路本地（fe80::/10）绑定时必须给
            // scope id**，不给就是 bind() 直接 EINVAL —— 不是"绑不上"，是"参数非法"，
            // 报错信息很容易把人往错的方向带。inet_pton 不认 %zone，所以自己切。
            // 有线 CarPlay 那条路绑的就是 NCM 网卡上的 fe80::2，必然走到这个分支。
            std::string host = bindAddr;
            const size_t pct = host.find('%');
            if (pct != std::string::npos) {
                const std::string zone = host.substr(pct + 1);
                host.resize(pct);
                const unsigned idx = ::if_nametoindex(zone.c_str());
                if (idx == 0) {
                    ::close(fd);
                    if (err) *err = "绑定地址里的网卡名不认：" + zone;
                    return false;
                }
                addr.sin6_scope_id = idx;
            } else if (host.rfind("fe80:", 0) == 0 || host.rfind("FE80:", 0) == 0) {
                ::close(fd);
                if (err) *err = "链路本地地址必须带 %网卡（不然 bind 会 EINVAL）：" + bindAddr;
                return false;
            }
            // 支持 IPv6 地址，或 IPv4 点分（写入 v4-mapped 的低 4 字节）
            if (::inet_pton(AF_INET6, host.c_str(), &addr.sin6_addr) != 1 &&
                ::inet_pton(AF_INET, host.c_str(), &addr.sin6_addr.s6_addr[12]) != 1) {
                ::close(fd);
                if (err) *err = "非法绑定地址：" + bindAddr;
                return false;
            }
        }
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(fd, 16) != 0) {
            int e = errno;
            ::close(fd);
            if (err) *err = "绑定/监听失败：" + std::string(std::strerror(e));
            return false;
        }

        fd_ = fd;
        if (port == 0) {
            struct sockaddr_in6 got{};
            socklen_t alen = sizeof got;
            if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&got), &alen) == 0)
                port_ = ntohs(got.sin6_port);
        } else {
            port_ = port;
        }
        return true;
    }

    std::unique_ptr<core::ITcpStream> accept(int timeoutMs) override
    {
        if (fd_ < 0) return nullptr;
        struct pollfd pfd{fd_, POLLIN, 0};
        int r = ::poll(&pfd, 1, timeoutMs < 0 ? -1 : timeoutMs);
        if (r <= 0) return nullptr;
        int cfd = ::accept(fd_, nullptr, nullptr);
        if (cfd < 0) return nullptr;
        setNoSigpipe(cfd);
        return std::make_unique<PosixTcpStream>(cfd);
    }

    uint16_t port() const override { return port_; }

    void stop() override
    {
        if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
    }

private:
    int      fd_   = -1;
    uint16_t port_ = 0;
};

// ---------------------------------------------------------------------------
// UDP socket（音频 RTP/RTCP）
// ---------------------------------------------------------------------------
class PosixUdpSocket : public core::IUdpSocket {
public:
    ~PosixUdpSocket() override { close(); }

    bool bind(uint16_t port, std::string* err) override
    {
        int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);   // 双栈 UDP
        if (fd < 0) {
            if (err) *err = "udp socket 失败：" + std::string(std::strerror(errno));
            return false;
        }
        int zero = 0;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        struct sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons(port);
        addr.sin6_addr   = in6addr_any;
        if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) != 0) {
            int e = errno;
            ::close(fd);
            if (err) *err = "udp bind 失败：" + std::string(std::strerror(e));
            return false;
        }
        fd_ = fd;
        if (port == 0) {
            struct sockaddr_in6 got{};
            socklen_t alen = sizeof got;
            if (::getsockname(fd_, reinterpret_cast<struct sockaddr*>(&got), &alen) == 0)
                port_ = ntohs(got.sin6_port);
        } else {
            port_ = port;
        }
        return true;
    }

    int recv(uint8_t* buf, size_t n, int timeoutMs) override
    {
        if (fd_ < 0) return -1;
        struct pollfd pfd{fd_, POLLIN, 0};
        const int r = ::poll(&pfd, 1, timeoutMs < 0 ? -1 : timeoutMs);
        if (r <= 0) return r;   // 0 = 超时；-1 = 错误
        peerLen_ = sizeof peer_;
        const ssize_t got = ::recvfrom(fd_, buf, n, 0,
                                       reinterpret_cast<struct sockaddr*>(&peer_), &peerLen_);
        return got < 0 ? -1 : int(got);
    }

    bool send(const uint8_t* buf, size_t n) override
    {
        if (fd_ < 0 || peerLen_ == 0) return false;
        return ::sendto(fd_, buf, n, 0,
                        reinterpret_cast<struct sockaddr*>(&peer_), peerLen_) == ssize_t(n);
    }

    uint16_t port() const override { return port_; }
    void close() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

private:
    int                 fd_   = -1;
    uint16_t            port_ = 0;
    struct sockaddr_storage peer_{};
    socklen_t           peerLen_ = 0;
};

// ---------------------------------------------------------------------------
// 网卡信息
// ---------------------------------------------------------------------------
std::string ipv4Of(struct ifaddrs* ifa)
{
    auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
    char  buf[INET_ADDRSTRLEN] = "";
    if (!inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof buf)) return {};
    return buf;
}

#if !defined(__APPLE__)
/// 读一个 sysfs 小文本文件并去掉尾部换行（失败返回空串）。
std::string readSmallFile(const std::string& path)
{
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return {};
    char         buf[256] = "";
    const size_t n        = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}
#endif

class PosixNetworkInfo : public core::INetworkInfo {
public:
    std::string pickInterfaceIPv4(std::string* ifnameOut) override
    {
        if (ifnameOut) ifnameOut->clear();
        struct ifaddrs* head = nullptr;
        if (getifaddrs(&head) != 0) return {};

        std::string bestIp, bestName;
        for (struct ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
            std::string ip = ipv4Of(ifa);
            if (ip.empty()) continue;
            bestIp = ip;
            bestName = ifa->ifa_name;
            break;   // 第一个够（lo 已排除；多网卡用 --ifname 显式指定）
        }
        freeifaddrs(head);
        if (ifnameOut) *ifnameOut = bestName;
        return bestIp;
    }

    std::string interfaceIPv4(const std::string& ifname) override
    {
        struct ifaddrs* head = nullptr;
        if (getifaddrs(&head) != 0) return {};
        std::string out;
        for (struct ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (ifname != ifa->ifa_name) continue;
            out = ipv4Of(ifa);
            break;
        }
        freeifaddrs(head);
        return out;
    }

    bool interfaceMAC(const std::string& ifname, uint8_t mac[6]) override
    {
#if defined(__APPLE__)
        struct ifaddrs* head = nullptr;
        if (getifaddrs(&head) != 0) return false;
        bool ok = false;
        for (struct ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
            if (ifname != ifa->ifa_name) continue;
            auto* dl = reinterpret_cast<struct sockaddr_dl*>(ifa->ifa_addr);
            if (dl->sdl_alen == 6) {
                std::memcpy(mac, LLADDR(dl), 6);
                ok = true;
            }
            break;
        }
        freeifaddrs(head);
        return ok;
#else
        struct ifaddrs* head = nullptr;
        if (getifaddrs(&head) != 0) return false;
        bool ok = false;
        for (struct ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_PACKET) continue;
            if (ifname != ifa->ifa_name) continue;
            auto* ll = reinterpret_cast<struct sockaddr_ll*>(ifa->ifa_addr);
            if (ll->sll_halen == 6) {
                std::memcpy(mac, ll->sll_addr, 6);
                ok = true;
            }
            break;
        }
        freeifaddrs(head);
        if (!ok) {
            int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0) return false;
            struct ifreq ifr{};
            std::snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname.c_str());
            if (::ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
                std::memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
                ok = true;
            }
            ::close(fd);
        }
        return ok;
#endif
    }

    std::string interfaceForUsbDevice(int bus, int address) override
    {
#if defined(__APPLE__)
        // macOS 没有内核 cdc_ncm（有线 CarPlay 那条路走的是 Apple 自家栈），
        // 这里给不出网卡名 —— 调用方会退回到"显式指定接口"。
        (void)bus;
        (void)address;
        return {};
#else
        if (bus <= 0 || address <= 0) return {};
        DIR* d = ::opendir("/sys/class/net");
        if (d == nullptr) return {};

        const std::string wantBus  = std::to_string(bus);
        const std::string wantAddr = std::to_string(address);
        std::string       found;
        while (struct dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == ".." || name == "lo") continue;
            // /sys/class/net/<n>/device 是指向 USB **接口**目录的符号链接
            // （形如 …/usb1/1-3/1-3:6.3）；它的父目录才是 USB 设备本身，
            // busnum/devnum 就在那里 —— 和 libusb 报的 bus/address 是同一对数。
            const std::string usbDev = "/sys/class/net/" + name + "/device/..";
            if (readSmallFile(usbDev + "/busnum") != wantBus) continue;
            if (readSmallFile(usbDev + "/devnum") != wantAddr) continue;
            found = name;
            break;
        }
        ::closedir(d);
        return found;
#endif
    }

    std::vector<std::string> interfaceIPv6LinkLocal(const std::string& ifname) override
    {
        std::vector<std::string> out;
        if (ifname.empty()) return out;

        struct ifaddrs* head = nullptr;
        if (getifaddrs(&head) != 0) return out;
        for (struct ifaddrs* ifa = head; ifa; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET6) continue;
            if (ifname != ifa->ifa_name) continue;
            auto* sa = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
            if (!IN6_IS_ADDR_LINKLOCAL(&sa->sin6_addr)) continue;
            char buf[INET6_ADDRSTRLEN] = "";
            if (inet_ntop(AF_INET6, &sa->sin6_addr, buf, sizeof buf) == nullptr) continue;
            out.emplace_back(buf);
        }
        freeifaddrs(head);
        return out;
    }
};

}  // namespace

core::INetworkInfo& networkInfo()
{
    static PosixNetworkInfo s_net;
    return s_net;
}

std::unique_ptr<core::ITcpListener> makeTcpListener()
{
    return std::make_unique<PosixTcpListener>();
}

std::unique_ptr<core::IUdpSocket> makeUdpSocket()
{
    return std::make_unique<PosixUdpSocket>();
}

// ---------------------------------------------------------------------------
// 有线 CarPlay 的媒体面（内核 cdc_ncm）
// ---------------------------------------------------------------------------

namespace {

/// 跑一条 shell 命令，输出收进 out。返回退出码（-1 = 起不来进程）。
/// 为什么走 shell 而不是 netlink：这里要干的活（摘 NetworkManager、拉 up、钉地址带
/// nodad）在 ip/nmcli 里都是一行，自己写 netlink 得几百行且更容易写错。这是**开机
/// bring-up 路径**，不是热路径，一次进程开销无所谓。
int runCmd(const std::string& cmd, std::string* out)
{
    const std::string full = cmd + " 2>&1";
    std::FILE*        f    = ::popen(full.c_str(), "r");
    if (f == nullptr) return -1;

    std::string  acc;
    char         buf[256];
    size_t       n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) acc.append(buf, n);
    const int status = ::pclose(f);
    if (out != nullptr) *out = acc;

    if (status < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

std::string trimNewlines(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

}  // namespace

bool prepareWiredNcm(int usbBus, int usbAddress, const std::string& wantLinkLocal,
                     WiredNcmLink* out, std::string* err)
{
    if (out == nullptr) return false;
    *out = WiredNcmLink{};

    const std::string ifname = networkInfo().interfaceForUsbDevice(usbBus, usbAddress);
    if (ifname.empty()) {
        if (err != nullptr) {
            *err = "找不到 USB " + std::to_string(usbBus) + "-" + std::to_string(usbAddress) +
                   " 挂出来的网卡（内核 cdc_ncm 建了没有？设备进 CarPlay 配置了吗？）";
        }
        return false;
    }
    out->ifname = ifname;

    uint8_t mac[6];
    if (networkInfo().interfaceMAC(ifname, mac)) out->mac = core::deviceIdFromMAC(mac);

    const std::string want = wantLinkLocal.empty() ? std::string("fe80::2") : wantLinkLocal;

    // ① 先把 NetworkManager 摘开。它会把这根线当"有线连接"去要 DHCP（点对点链路上
    //    根本没有 DHCP 服务器），一直卡在 connecting，并且随时可能清掉我们钉的地址。
    //    摘不掉不算致命（有些系统没装 NM），继续往下走。
    std::string log;
    if (runCmd("nmcli device set '" + ifname + "' managed no", &log) != 0) {
        LOG_INFO("[wired-ncm] nmcli 摘管失败（没装 NM？不影响）：%s", trimNewlines(log).c_str());
    } else {
        out->unmanaged = true;
    }

    // ② 拉 up。网卡刚被内核建出来时默认是 DOWN，DOWN 的链路上钉不了地址。
    if (runCmd("ip link set dev '" + ifname + "' up", &log) != 0) {
        if (err != nullptr) *err = "ip link set up 失败：" + trimNewlines(log);
        return false;
    }
    out->broughtUp = true;

    // ③ 链路上有没有可用的链路本地地址？没有就自己钉一个。
    //    判据是"读回来看"，不是"命令退出码"—— 重复 add 会报 EEXIST，那不算失败。
    out->ipv6 = networkInfo().interfaceIPv6LinkLocal(ifname);
    bool hasWant = false;
    for (const std::string& a : out->ipv6)
        if (a == want) hasWant = true;

    if (!hasWant) {
        if (runCmd("ip -6 addr add " + want + "/64 dev '" + ifname + "' nodad", &log) != 0) {
            if (err != nullptr) {
                *err = "钉 " + want + "/64 失败：" + trimNewlines(log);
            }
            return false;
        }
        out->pinned = true;
        out->ipv6   = networkInfo().interfaceIPv6LinkLocal(ifname);
    }

    // ④ 钉的那个排第一 —— 调用方（0x4301）只用第一个，得保证它是我们指定的那个。
    for (size_t i = 0; i < out->ipv6.size(); ++i) {
        if (out->ipv6[i] == want) {
            std::swap(out->ipv6[0], out->ipv6[i]);
            break;
        }
    }

    if (out->ipv6.empty()) {
        if (err != nullptr) *err = ifname + " 上没有 IPv6 链路本地地址（钉完还是没有？）";
        return false;
    }
    LOG_INFO("[wired-ncm] %s：MAC %s，IPv6 %s（%s），NM %s", ifname.c_str(), out->mac.c_str(),
             out->ipv6.front().c_str(), out->pinned ? "这次新钉的" : "原本就有",
             out->unmanaged ? "已摘开" : "没动");
    return true;
}

core::PlatformServices makePlatformServices()
{
    core::PlatformServices s;
    s.network           = &networkInfo();
    s.makeTcpListener   = &makeTcpListener;
    s.makeMdnsAdvertiser = &makeMdnsAdvertiser;
    s.makeUdpSocket     = &makeUdpSocket;
    s.bluetooth         = nullptr;   // 蓝牙触发由组合根显式装配（见 makeBluetooth）
    return s;
}

}  // namespace allplay::posix
