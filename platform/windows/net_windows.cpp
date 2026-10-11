// ============================================================================
// platform/windows/net_windows.cpp —— Winsock2 适配（TCP/UDP/网卡）
// ============================================================================
// 对照 platform/posix/net_posix.cpp 的语义：
//   read 用 select() 轮询超时；accept 用 select()；stop() 关 socket 唤醒阻塞。
// 双栈：IPv6 socket 设 IPV6_V6ONLY=0，同时收 IPv4（iPhone CarPlay 常走 IPv6）。
#include "windows/net_windows.h"

#include "common/log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <cstring>
#include <mutex>

namespace allplay::windows {

namespace {

bool g_wsa = false;
std::mutex g_wsaMu;

int waitReadable(SOCKET s, int timeoutMs)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    timeval  tv{};
    timeval* ptv = nullptr;
    if (timeoutMs >= 0) {
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        ptv = &tv;
    }
    return ::select(0, &fds, nullptr, nullptr, ptv);   // >0 可读；0 超时；SOCKET_ERROR 错
}

// WCHAR* → UTF-8 std::string
std::string wideToUtf8(const wchar_t* w)
{
    if (!w) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// TCP 流
// ---------------------------------------------------------------------------
class WinTcpStream : public core::ITcpStream {
public:
    explicit WinTcpStream(SOCKET s) : s_(s) {}
    ~WinTcpStream() override { close(); }
    WinTcpStream(const WinTcpStream&) = delete;

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        if (s_ == INVALID_SOCKET) return -1;
        const int r = waitReadable(s_, timeoutMs);
        if (r == 0) return 0;                 // 超时
        if (r == SOCKET_ERROR) return -1;
        const int got = ::recv(s_, reinterpret_cast<char*>(buf), int(n), 0);
        if (got > 0) return got;
        if (got == 0) return -1;              // 对端关闭
        return -1;
    }

    bool writeAll(const uint8_t* buf, size_t n) override
    {
        if (s_ == INVALID_SOCKET) return false;
        size_t off = 0;
        while (off < n) {
            const int w = ::send(s_, reinterpret_cast<const char*>(buf + off), int(n - off), 0);
            if (w == SOCKET_ERROR || w == 0) return false;
            off += size_t(w);
        }
        return true;
    }

    void close() override
    {
        if (s_ != INVALID_SOCKET) { ::closesocket(s_); s_ = INVALID_SOCKET; }
    }

private:
    SOCKET s_ = INVALID_SOCKET;
};

// ---------------------------------------------------------------------------
// TCP 监听
// ---------------------------------------------------------------------------
class WinTcpListener : public core::ITcpListener {
public:
    ~WinTcpListener() override { stop(); }

    bool start(const std::string& bindAddr, uint16_t port, std::string* err) override
    {
        SOCKET s = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) {
            if (err) *err = "socket 失败：" + std::to_string(WSAGetLastError());
            return false;
        }
        int one = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
        int zero = 0;
        ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&zero), sizeof zero);

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons(port);
        addr.sin6_addr   = in6addr_any;
        if (!bindAddr.empty()) {
            if (::inet_pton(AF_INET6, bindAddr.c_str(), &addr.sin6_addr) != 1 &&
                ::inet_pton(AF_INET, bindAddr.c_str(), &addr.sin6_addr.u.Byte[12]) != 1) {
                ::closesocket(s);
                if (err) *err = "非法绑定地址：" + bindAddr;
                return false;
            }
        }
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR ||
            ::listen(s, 16) == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            ::closesocket(s);
            if (err) *err = "绑定/监听失败：" + std::to_string(e);
            return false;
        }

        s_ = s;
        if (port == 0) {
            sockaddr_in6 got{};
            int alen = sizeof got;
            if (::getsockname(s_, reinterpret_cast<sockaddr*>(&got), &alen) == 0)
                port_ = ntohs(got.sin6_port);
        } else {
            port_ = port;
        }
        return true;
    }

    std::unique_ptr<core::ITcpStream> accept(int timeoutMs) override
    {
        if (s_ == INVALID_SOCKET) return nullptr;
        const int r = waitReadable(s_, timeoutMs);
        if (r <= 0) return nullptr;
        SOCKET c = ::accept(s_, nullptr, nullptr);
        if (c == INVALID_SOCKET) return nullptr;
        return std::make_unique<WinTcpStream>(c);
    }

    uint16_t port() const override { return port_; }

    void stop() override
    {
        if (s_ != INVALID_SOCKET) {
            ::shutdown(s_, SD_BOTH);
            ::closesocket(s_);
            s_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET   s_    = INVALID_SOCKET;
    uint16_t port_ = 0;
};

// ---------------------------------------------------------------------------
// UDP socket（音频 RTP/RTCP）
// ---------------------------------------------------------------------------
class WinUdpSocket : public core::IUdpSocket {
public:
    ~WinUdpSocket() override { close(); }

    bool bind(uint16_t port, std::string* err) override
    {
        SOCKET s = ::socket(AF_INET6, SOCK_DGRAM, 0);
        if (s == INVALID_SOCKET) {
            if (err) *err = "udp socket 失败：" + std::to_string(WSAGetLastError());
            return false;
        }
        int zero = 0;
        ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&zero), sizeof zero);
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons(port);
        addr.sin6_addr   = in6addr_any;
        if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            ::closesocket(s);
            if (err) *err = "udp bind 失败：" + std::to_string(e);
            return false;
        }
        s_ = s;
        if (port == 0) {
            sockaddr_in6 got{};
            int alen = sizeof got;
            if (::getsockname(s_, reinterpret_cast<sockaddr*>(&got), &alen) == 0)
                port_ = ntohs(got.sin6_port);
        } else {
            port_ = port;
        }
        return true;
    }

    int recv(uint8_t* buf, size_t n, int timeoutMs) override
    {
        if (s_ == INVALID_SOCKET) return -1;
        const int r = waitReadable(s_, timeoutMs);
        if (r <= 0) return r;   // 0 超时；SOCKET_ERROR 错误
        peerLen_ = sizeof peer_;
        const int got = ::recvfrom(s_, reinterpret_cast<char*>(buf), int(n), 0,
                                   reinterpret_cast<sockaddr*>(&peer_), &peerLen_);
        return got == SOCKET_ERROR ? -1 : got;
    }

    bool send(const uint8_t* buf, size_t n) override
    {
        if (s_ == INVALID_SOCKET || peerLen_ == 0) return false;
        const int w = ::sendto(s_, reinterpret_cast<const char*>(buf), int(n), 0,
                               reinterpret_cast<sockaddr*>(&peer_), peerLen_);
        return w == int(n);
    }

    uint16_t port() const override { return port_; }
    void close() override { if (s_ != INVALID_SOCKET) { ::closesocket(s_); s_ = INVALID_SOCKET; } }

private:
    SOCKET           s_    = INVALID_SOCKET;
    uint16_t         port_ = 0;
    sockaddr_storage peer_{};
    int              peerLen_ = 0;
};

// ---------------------------------------------------------------------------
// 网卡信息（GetAdaptersAddresses）
// ---------------------------------------------------------------------------
class WinNetworkInfo : public core::INetworkInfo {
public:
    std::string pickInterfaceIPv4(std::string* ifnameOut) override
    {
        if (ifnameOut) ifnameOut->clear();
        ULONG size = 0;
        ::GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, nullptr, &size);
        if (size == 0) return {};
        std::vector<BYTE> buf(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (::GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapters, &size) != NO_ERROR)
            return {};

        for (auto* a = adapters; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            std::string ip = firstIPv4(a);
            if (ip.empty()) continue;
            if (ifnameOut) *ifnameOut = wideToUtf8(a->FriendlyName);
            return ip;
        }
        return {};
    }

    std::string interfaceIPv4(const std::string& ifname) override
    {
        ULONG size = 0;
        ::GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, nullptr, &size);
        if (size == 0) return {};
        std::vector<BYTE> buf(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (::GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapters, &size) != NO_ERROR)
            return {};
        for (auto* a = adapters; a; a = a->Next) {
            if (ifname != wideToUtf8(a->FriendlyName)) continue;
            return firstIPv4(a);
        }
        return {};
    }

    bool interfaceMAC(const std::string& ifname, uint8_t mac[6]) override
    {
        ULONG size = 0;
        ::GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, nullptr, &size);
        if (size == 0) return false;
        std::vector<BYTE> buf(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (::GetAdaptersAddresses(AF_UNSPEC, 0, nullptr, adapters, &size) != NO_ERROR)
            return false;
        for (auto* a = adapters; a; a = a->Next) {
            if (ifname != wideToUtf8(a->FriendlyName)) continue;
            if (a->PhysicalAddressLength < 6) return false;
            std::memcpy(mac, a->PhysicalAddress, 6);
            return true;
        }
        return false;
    }

private:
    static std::string firstIPv4(IP_ADAPTER_ADDRESSES* a)
    {
        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            auto* sa = ua->Address.lpSockaddr;
            if (!sa || sa->sa_family != AF_INET) continue;
            auto* in = reinterpret_cast<sockaddr_in*>(sa);
            char buf[INET_ADDRSTRLEN] = "";
            ::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf);
            return buf;
        }
        return {};
    }
};

// ---------------------------------------------------------------------------
// mDNS 桩（Windows 待接 Bonjour/mDNSResponder；start 失败不致命）
// ---------------------------------------------------------------------------
class WinMdnsStub : public core::IMdnsAdvertiser {
public:
    bool start(const std::string&, uint16_t, const std::vector<std::string>&,
               std::string* err) override
    {
        if (err) *err = "Windows mDNS 未实现（待接 Bonjour/mDNSResponder）";
        return false;
    }
    void stop() override {}
};

}  // namespace

bool initWinsock(std::string* err)
{
    std::lock_guard<std::mutex> lk(g_wsaMu);
    if (g_wsa) return true;
    WSADATA d{};
    if (::WSAStartup(MAKEWORD(2, 2), &d) != 0) {
        if (err) *err = "WSAStartup 失败：" + std::to_string(WSAGetLastError());
        return false;
    }
    g_wsa = true;
    return true;
}

core::INetworkInfo& networkInfo()
{
    static WinNetworkInfo g_net;
    return g_net;
}

std::unique_ptr<core::ITcpListener> makeTcpListener()
{
    return std::make_unique<WinTcpListener>();
}

std::unique_ptr<core::IUdpSocket> makeUdpSocket()
{
    return std::make_unique<WinUdpSocket>();
}

std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser()
{
    return std::make_unique<WinMdnsStub>();
}

core::PlatformServices makePlatformServices()
{
    core::PlatformServices s;
    s.network             = &networkInfo();
    s.makeTcpListener     = &makeTcpListener;
    s.makeMdnsAdvertiser  = &makeMdnsAdvertiser;
    s.makeUdpSocket       = &makeUdpSocket;
    s.bluetooth           = nullptr;   // 蓝牙 iAP2 触发后续里程碑
    return s;
}

}  // namespace allplay::windows
