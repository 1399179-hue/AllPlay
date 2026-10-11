// ============================================================================
// platform/posix/mdns_dns_sd.cpp —— macOS 后端：系统自带 mDNSResponder（dns_sd.h）
// ============================================================================
// macOS 上 mDNSResponder 常驻系统，dns_sd 直接可用，不需要 avahi 这类服务，
// 也不需要密码/权限。TXT 记录用 TXTRecord* API 正经编码（length-prefixed）。
//
// 事件循环：DNSServiceRefSockFD 拿到 fd + select 超时轮询跑 DNSServiceProcessResult。
// （不用 DNSServiceSetDispatchQueue，省掉 libdispatch 依赖。）
#include "posix/posix.h"

#include "common/log.h"

#include <dns_sd.h>

#include <arpa/inet.h>   // htons
#include <sys/select.h>
#include <unistd.h>

#include <atomic>
#include <thread>

namespace allplay::posix {

namespace {

class DnsSdBonjour : public core::IMdnsAdvertiser {
public:
    ~DnsSdBonjour() override { stop(); }

    bool start(const std::string& name, uint16_t port,
               const std::vector<std::string>& txt, std::string* err) override
    {
        // "k=v" → TXT（v 里允许含 '='，只按第一个 '=' 切）
        TXTRecordRef rec;
        TXTRecordCreate(&rec, 0, nullptr);
        for (const auto& kv : txt) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            std::string k = kv.substr(0, eq);
            std::string v = kv.substr(eq + 1);
            TXTRecordSetValue(&rec, k.c_str(), (uint8_t)v.size(), v.data());
        }

        DNSServiceErrorType e = DNSServiceRegister(
            &ref_, /*flags=*/0, /*ifIndex=*/0,
            name.c_str(), "_airplay._tcp", /*domain=*/nullptr, /*host=*/nullptr,
            htons(port),
            TXTRecordGetLength(&rec), TXTRecordGetBytesPtr(&rec),
            /*callback=*/nullptr, /*context=*/nullptr);
        TXTRecordDeallocate(&rec);

        if (e != kDNSServiceErr_NoError) {
            // dns_sd 没有公开的 error→string；打错误码即可
            if (err) *err = "DNSServiceRegister 失败：errno=" + std::to_string(int(e));
            ref_ = nullptr;
            return false;
        }

        fd_ = DNSServiceRefSockFD(ref_);
        if (fd_ < 0) {
            if (err) *err = "DNSServiceRefSockFD 失败";
            DNSServiceRefDeallocate(ref_);
            ref_ = nullptr;
            return false;
        }

        name_ = name;
        port_ = port;
        stop_.store(false);
        thread_ = std::thread([this] { loop(); });
        LOG_INFO("Bonjour 已广播（dns_sd）：%s._airplay._tcp :%u", name_.c_str(), port_);
        return true;
    }

    void stop() override
    {
        if (!ref_) return;
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        DNSServiceRefDeallocate(ref_);
        ref_ = nullptr;
        fd_ = -1;
    }

private:
    void loop()
    {
        while (!stop_.load()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd_, &set);
            timeval tv{0, 200 * 1000};   // 200ms
            int r = ::select(fd_ + 1, &set, nullptr, nullptr, &tv);
            if (r > 0 && FD_ISSET(fd_, &set)) {
                if (DNSServiceProcessResult(ref_) != kDNSServiceErr_NoError) break;
            }
        }
    }

    DNSServiceRef     ref_ = nullptr;
    int               fd_  = -1;
    std::string       name_;
    uint16_t          port_ = 0;
    std::atomic<bool> stop_{true};
    std::thread       thread_;
};

}  // namespace

std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser()
{
    return std::unique_ptr<core::IMdnsAdvertiser>(new DnsSdBonjour);
}

}  // namespace allplay::posix
