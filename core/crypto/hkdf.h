/**
 * @file   hkdf.h
 * @brief  HKDF-SHA512 + SHA1/256/512（OpenSSL 实现）
 * @author blt
 */

// ============================================================================
// core/crypto/hkdf.h —— HKDF-SHA512 + SHA1/256/512（OpenSSL 实现）
// ============================================================================
// AirPlay 各处的密钥导出都用 HKDF-SHA512（标准 RFC 5869：Extract + Expand）。
// salt 作为 HKDF 的 salt 参数传入（不是拼到数据上）——对照标准 HKDF（RFC 5869）
// 的 Extract + Expand 参数用法（ikm / salt / info）。
// SHA1 只用于 MFi-SAP 的 AES-KEY/AES-IV 派生。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace core::crypto {

/// HKDF-SHA512（RFC 5869）。salt 可为空（等效 64 字节零）。
std::vector<uint8_t> hkdfSha512(const uint8_t* ikm, size_t ikmLen,
                                const uint8_t* salt, size_t saltLen,
                                const uint8_t* info, size_t infoLen,
                                size_t outLen = 32);

/// 便捷：ikm 用 vector，salt/info 用原始字节。
std::vector<uint8_t> hkdfSha512(const std::vector<uint8_t>& ikm,
                                const std::vector<uint8_t>& salt,
                                const std::vector<uint8_t>& info,
                                size_t outLen = 32);

/// 便捷：salt/info 用 ASCII 字符串。
std::vector<uint8_t> hkdfSha512(const std::vector<uint8_t>& ikm,
                                const std::string& salt,
                                const std::string& info,
                                size_t outLen = 32);

std::vector<uint8_t> sha1(const std::vector<uint8_t>& data);
std::vector<uint8_t> sha256(const std::vector<uint8_t>& data);
std::vector<uint8_t> sha512(const std::vector<uint8_t>& data);

}  // namespace core::crypto
