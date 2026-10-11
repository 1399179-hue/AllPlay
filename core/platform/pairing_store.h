/**
 * @file   pairing_store.h
 * @brief  手机（controller）配对记录的持久化
 * @author blt
 */

// ============================================================================
// core/platform/pairing_store.h —— 手机（controller）配对记录的持久化
// ============================================================================
// pair-setup M5 存手机 Ed25519 长期公钥（LTPK）+ 其 identifier（UUID 字符串）；
// pair-verify M3 按 identifier 取回验签。首版只存"最近一个"手机（单手机场景），
// 多手机映射属 M4+ 细化。落盘 0600，与 device_identity 同目录。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace core {

class PairingStore {
public:
    /// 从 stateDir 读（不存在/读不出返回 false，不报错）。
    bool load(const std::string& stateDir);

    bool hasPairing() const { return !controllerId_.empty(); }
    const std::string&        controllerId() const { return controllerId_; }
    const std::vector<uint8_t>& controllerLtpk() const { return controllerLtpk_; }

    /// 保存（0600）。失败返回 false。
    bool save(const std::string& stateDir, const std::string& id,
              const std::vector<uint8_t>& ltpk32);

private:
    std::string         controllerId_;
    std::vector<uint8_t> controllerLtpk_;
};

}  // namespace core
