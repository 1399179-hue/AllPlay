/**
 * @file   aes_ctr.h
 * @brief  AES-128-CTR（MFi-SAP 签密用）
 * @author blt
 */

// ============================================================================
// core/crypto/aes_ctr.h —— AES-128-CTR（MFi-SAP 签密用）
// ============================================================================
// /auth-setup 里我方签名用 AES-128-CTR 加密（key/iv 由 SHA1("AES-KEY"/"AES-IV"||shared)
// 派生）。CTR 模式加/解密是同一个操作。
#pragma once

#include <cstdint>
#include <vector>
#include <cstddef>

namespace core::crypto {

std::vector<uint8_t> aes128Ctr(const uint8_t key[16], const uint8_t iv[16],
                               const uint8_t* data, size_t len);

}  // namespace core::crypto
