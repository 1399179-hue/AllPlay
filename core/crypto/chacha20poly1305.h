/**
 * @file   chacha20poly1305.h
 * @brief  ChaCha20-Poly1305 AEAD（RFC 8439）
 * @author blt
 */

// ============================================================================
// core/crypto/chacha20poly1305.h —— ChaCha20-Poly1305 AEAD（RFC 8439）
// ============================================================================
// AirPlay 的加密帧都用它：128 位 tag、12 字节 nonce。nonce 有两种来源：
//   nonce64(counter)  前 4 字节零 + 8 字节小端计数器（控制/数据流，双向独立）
//   nonceLabel(label) 前 4 字节零 + 后 8 字节放 label 的 ASCII（pair-verify 封内层）
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <cstddef>

namespace core::crypto {

/// nonce = 前 4 字节零 + 8 字节小端计数器（初始 0，逐帧 ++）。
std::array<uint8_t, 12> nonce64(uint64_t counter);

/// nonce = 前 4 字节零 + 后 8 字节放 label 的 ASCII（不足 8 字节后面补零）。
std::array<uint8_t, 12> nonceLabel(const std::string& label);

/// 加密：输出 = 密文 ‖ 16 字节 tag。
std::vector<uint8_t> chachaSeal(const uint8_t key[32], const std::array<uint8_t, 12>& nonce,
                                const uint8_t* aad, size_t aadLen,
                                const uint8_t* pt, size_t ptLen);

/// 解密：ct 的最后 16 字节是 tag。成功写 out 并返回 true（tag 校验失败返回 false）。
bool chachaOpen(const uint8_t key[32], const std::array<uint8_t, 12>& nonce,
                const uint8_t* aad, size_t aadLen,
                const uint8_t* ct, size_t ctLen,
                std::vector<uint8_t>* out);

}  // namespace core::crypto
