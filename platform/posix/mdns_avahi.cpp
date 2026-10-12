// ============================================================================
// platform/posix/mdns_avahi.cpp —— Linux 后端：经 D-Bus 调系统 avahi-daemon
// ============================================================================
// avahi-daemon 没跑时不致命：start() 返回 false 并给出原因（RTSP 照常监听）。
#include "posix/posix.h"

#include "common/log.h"

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/strlst.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace allplay::posix {

namespace {

class AvahiBonjour : public core::IMdnsAdvertiser {
public:
    ~AvahiBonjour() override { stop(); }

    bool start(const std::string& name, uint16_t port,
               const std::vector<std::string>& txt, std::string* err) override
    {
        name_ = name;
        port_ = port;
        txt_  = txt;

        poll_ = avahi_simple_poll_new();
        if (!poll_) {
            if (err) *err = "avahi_simple_poll_new 失败";
            return false;
        }
        int aerr = 0;
        // NO_FAIL：daemon 没跑时不立刻失败（进 FAILURE 状态后我们不提交，见回调）
        client_ = avahi_client_new(avahi_simple_poll_get(poll_), AVAHI_CLIENT_NO_FAIL,
                                   &AvahiBonjour::clientCb, this, &aerr);
        if (!client_) {
            if (err) *err = std::string("连 avahi-daemon 失败：") + avahi_strerror(aerr);
            avahi_simple_poll_free(poll_);
            poll_ = nullptr;
            return false;
        }

        stop_.store(false);
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                if (avahi_simple_poll_iterate(poll_, 200) != 0) break;
            }
        });

        // 等广播真正提交（最多 ~0.5s），好把结果如实报告给调用方
        for (int i = 0; i < 25; ++i) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (published_) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (err) *err = "avahi 未在 0.5s 内进入 RUNNING（daemon 没跑？）";
        return false;
    }

    void stop() override
    {
        stop_.store(true);
        if (poll_) avahi_simple_poll_quit(poll_);
        if (thread_.joinable()) thread_.join();

        std::lock_guard<std::mutex> lk(mu_);
        if (group_)  { avahi_entry_group_free(group_); group_ = nullptr; }
        if (client_) { avahi_client_free(client_);     client_ = nullptr; }
        if (poll_)   { avahi_simple_poll_free(poll_);  poll_ = nullptr; }
    }

private:
    static void clientCb(AvahiClient* c, AvahiClientState state, void* userdata)
    {
        auto* self = static_cast<AvahiBonjour*>(userdata);
        if (state != AVAHI_CLIENT_S_RUNNING) return;

        std::lock_guard<std::mutex> lk(self->mu_);
        if (!self->group_) {
            self->group_ = avahi_entry_group_new(c, nullptr, nullptr);
            if (!self->group_) {
                LOG_WARN("avahi：建 entry group 失败：%s", avahi_strerror(avahi_client_errno(c)));
                return;
            }
        }
        if (avahi_entry_group_is_empty(self->group_)) {
            // 注意：avahi_entry_group_add_service() 是可变参数接口，它把 `...` 当成
            // 一串以 NULL 结尾的 const char*。传 cstr.data()（一个 const char**）会让
            // avahi 把**指针数组自身的内存**当字符串读，TXT 记录变成几个字节的二进制垃圾
            // ——iPhone 读不到 features/flags/deviceid 就不会来连 AirPlay。
            // 必须用 strlst 版本，配 AvahiStringList。
            AvahiStringList* lst = nullptr;
            for (auto it = self->txt_.rbegin(); it != self->txt_.rend(); ++it)
                lst = avahi_string_list_add(lst, it->c_str());  // add 是头插，倒着加才是原序

            int r = avahi_entry_group_add_service_strlst(
                self->group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, (AvahiPublishFlags)0,
                self->name_.c_str(), "_airplay._tcp", nullptr, nullptr,
                self->port_, lst);
            avahi_string_list_free(lst);
            if (r < 0) {
                LOG_WARN("avahi：add_service 失败：%s", avahi_strerror(r));
                return;
            }
            if ((r = avahi_entry_group_commit(self->group_)) < 0) {
                LOG_WARN("avahi：commit 失败：%s", avahi_strerror(r));
                return;
            }
            self->published_ = true;
            LOG_INFO("Bonjour 已广播（avahi）：%s._airplay._tcp :%u",
                     self->name_.c_str(), self->port_);
        }
    }

    AvahiSimplePoll* poll_   = nullptr;
    AvahiClient*     client_ = nullptr;
    AvahiEntryGroup* group_  = nullptr;

    std::string              name_;
    uint16_t                 port_ = 0;
    std::vector<std::string> txt_;

    std::thread       thread_;
    std::atomic<bool> stop_{true};
    std::mutex        mu_;
    bool              published_ = false;
};

}  // namespace

std::unique_ptr<core::IMdnsAdvertiser> makeMdnsAdvertiser()
{
    return std::unique_ptr<core::IMdnsAdvertiser>(new AvahiBonjour);
}

}  // namespace allplay::posix
