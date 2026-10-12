/**
 * @file   srp6a.h
 * @brief  SRP-6a 服务端（RFC 5054 3072 位，SHA-512，AirPlay 变体）
 * @author blt
 */

// ============================================================================
// core/crypto/srp6a.h —— SRP-6a 服务端（RFC 5054 3072 位，SHA-512，AirPlay 变体）
// ============================================================================
// 参照 AirPlay SRP-6a 协议（RFC 5054；公式逐字节核对）：
//   username="Pair-Setup"、password="3939" 内定。
//   k = SHA512(toBytes(N) ‖ pad384(g))；u = SHA512(pad384(A) ‖ pad384(B))；
//   K = SHA512(toBytes(S))（最短字节，不补零、不做 RFC5054 交错哈希）——坑！
#pragma once

#include <openssl/bn.h>

#include <cstdint>
#include <vector>
#include <cstddef>

namespace core::crypto {

class Srp6aServer {
public:
    Srp6aServer();               ///< 生成盐 s 与 b，算好 B
    /// 固定盐/指数（确定性测试/交叉验证用；真机走默认随机构造）。
    Srp6aServer(const uint8_t salt[16], const uint8_t b[32]);
    ~Srp6aServer();
    Srp6aServer(const Srp6aServer&) = delete;
    Srp6aServer& operator=(const Srp6aServer&) = delete;

    const std::vector<uint8_t>& salt() const;      ///< 16 字节
    const std::vector<uint8_t>& publicKey() const; ///< pad(B)，384 字节

    /// 收到 A(384) 与客户端 proof M1(64)。成功写 K(64)/M2(64) 并返回 true。
    bool verify(const uint8_t* A, size_t aLen,
                const uint8_t* m1, size_t m1Len,
                std::vector<uint8_t>* K, std::vector<uint8_t>* m2);

private:
    void init(const uint8_t* salt, const uint8_t* b);

    BIGNUM* n_ = nullptr;   // 素数
    BIGNUM* g_ = nullptr;   // 生成元 5
    BIGNUM* k_ = nullptr;   // multiplier
    BIGNUM* v_ = nullptr;   // verifier
    BIGNUM* b_ = nullptr;   // 私钥指数
    BIGNUM* B_ = nullptr;   // 公钥 B

    std::vector<uint8_t> salt_;   // 16 字节
    std::vector<uint8_t> pubB_;   // pad(B) 384 字节
};

}  // namespace core::crypto
