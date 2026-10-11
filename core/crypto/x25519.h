/**
 * @file   x25519.h
 * @brief  X25519 密钥交换（OpenSSL）
 * @author blt
 */

// ============================================================================
// core/crypto/x25519.h —— X25519 密钥交换（OpenSSL）
// ============================================================================
// pair-verify 与 /auth-setup 都用临时 X25519 做 ECDH。
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace core::crypto {

struct X25519Key {
    std::array<uint8_t, 32> pub;
    std::array<uint8_t, 32> priv;
};

/// 随机生成临时密钥对。
X25519Key x25519Generate();

/// DH：priv(32) × peerPub(32) → 32 字节共享密钥；失败（或全零）返回空。
std::vector<uint8_t> x25519Shared(const uint8_t priv[32], const uint8_t peerPub[32]);

}  // namespace core::crypto
