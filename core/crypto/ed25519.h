/**
 * @file   ed25519.h
 * @brief  Ed25519 签/验（OpenSSL）
 * @author blt
 */

// ============================================================================
// core/crypto/ed25519.h —— Ed25519 签/验（OpenSSL）
// ============================================================================
// pair-setup/pair-verify 里：本机用长期 Ed25519 私钥签，用对端公钥（32 字节）验。
#pragma once

#include <openssl/evp.h>

#include <cstdint>
#include <vector>
#include <cstddef>

namespace core::crypto {

/// 用 EVP_PKEY(Ed25519) 私钥签名，返回 64 字节；失败返回空。
std::vector<uint8_t> ed25519Sign(EVP_PKEY* key, const uint8_t* msg, size_t len);

/// 用原始 32 字节公钥验签。
bool ed25519Verify(const uint8_t pub[32], const uint8_t* msg, size_t len, const uint8_t sig[64]);

}  // namespace core::crypto
