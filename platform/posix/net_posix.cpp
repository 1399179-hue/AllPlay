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
#include <unistd.h>

#if defined(__APPLE__)
#  include <net/if_dl.h>       // sockaddr_dl / LLADDR
#else
#  include <linux/if_packet.h> // sockaddr_ll（AF_PACKET）
#  include <linux/sockios.h>   // SIOCGIFHWADDR
#  include <sys/ioctl.h>
#endif

#include <cerrno>
#include <cstring>

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
            // 支持 IPv6 地址，或 IPv4 点分（写入 v4-mapped 的低 4 字节）
            if (::inet_pton(AF_INET6, bindAddr.c_str(), &addr.sin6_addr) != 1 &&
                ::inet_pton(AF_INET, bindAddr.c_str(), &addr.sin6_addr.s6_addr[12]) != 1) {
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
