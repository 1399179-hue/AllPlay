// ============================================================================
// platform/posix/bt_bluez.cpp —— Linux 桩（BlueZ 传输层待做）
// ============================================================================
// 为什么先不做：目标 Ubuntu 是 Mac 上的 VMware 虚拟机，虚拟蓝牙不可用
// （初始化 ENOSYS），真机蓝牙联调先在 Mac（IOBluetooth）上走。等有可用蓝牙的
// Linux 机器（USB dongle 直通）时，在这里用 libbluetooth 实现：
//   pairedDevices  → BlueZ D-Bus（org.bluez）或解析 /var/lib/bluetooth
//   connectRFCOMM  → SDP 查 UUID（libbluetooth 的 sdp_connect）+ RFCOMM socket
#include "posix/posix.h"

#include "common/log.h"

namespace allplay::posix {

namespace {

class LinuxBluetooth : public core::IBluetooth {
public:
    std::vector<core::BtDevice> pairedDevices(std::string* err) override
    {
        if (err) *err = "Linux 蓝牙传输层尚未实现（见 bt_bluez.cpp 顶部说明）";
        return {};
    }

    core::IByteStream* connectRFCOMM(const std::string&, const std::string&,
                                     std::string* err) override
    {
        if (err) *err = "Linux 蓝牙传输层尚未实现（见 bt_bluez.cpp 顶部说明）";
        return nullptr;
    }

    core::IByteStream* waitForPhoneConnection(int, std::string* err) override
    {
        if (err) *err = "Linux 蓝牙传输层尚未实现（见 bt_bluez.cpp 顶部说明）";
        return nullptr;
    }
};

}  // namespace

std::unique_ptr<core::IBluetooth> makeBluetooth()
{
    return std::make_unique<LinuxBluetooth>();
}

std::string controllerAddress() { return ""; }
std::string currentSSID() { return ""; }

}  // namespace allplay::posix
