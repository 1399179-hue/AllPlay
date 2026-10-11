/**
 * @file   bluetooth.h
 * @brief  蓝牙传输抽象（iAP2 触发用）
 * @author blt
 */

// ============================================================================
// core/port/bluetooth.h —— 蓝牙传输抽象（iAP2 触发用）
// ============================================================================
// 平台实现：macOS=IOBluetooth，Linux=BlueZ（待做），Android=BluetoothSocket。
// iAP2 协议栈（core/iap2）与平台无关，直接吃 core::IByteStream。
//
// ⚠️ macOS 27：**必须有蓝牙权限**才能枚举/查询；ssh 进程会被 TCC 静默拒绝。
#pragma once

#include "port/stream.h"

#include <memory>
#include <string>
#include <vector>

namespace core {

/// iPhone 上暴露的 iAP2 SPP 服务 UUID（客户端方向：我们主动连手机）
inline const char* const kIap2SppUuid = "00000000-deca-fade-deca-deafdecacafe";

/// 配件侧 "Wireless iAP" 服务 UUID（服务端方向：注册 SDP 等手机连）
inline const char* const kWirelessIapUuid = "00000000-deca-fade-deca-deafdecacaff";

struct BtDevice {
    std::string name;
    std::string address;    ///< 平台形态（macOS "AA-BB-CC-DD-EE-FF" 小写短横线）
    bool        connected = false;
};

class IBluetooth {
public:
    virtual ~IBluetooth() = default;

    /// 已配对设备（macOS 27 需蓝牙权限；列表空时 err 说明可能原因）。
    virtual std::vector<BtDevice> pairedDevices(std::string* err) = 0;

    /// SDP 查 uuid → 开 RFCOMM；失败返回 nullptr 并写 err。
    /// 返回的指针不归调用方所有（生命周期契约见各平台实现）。
    virtual IByteStream* connectRFCOMM(const std::string& address,
                                       const std::string& uuid,
                                       std::string* err) = 0;

    /// 服务端方向：注册 SDP 并阻塞等手机连入（超时返回 nullptr）。
    virtual IByteStream* waitForPhoneConnection(int timeoutMs, std::string* err) = 0;
};

}  // namespace core
