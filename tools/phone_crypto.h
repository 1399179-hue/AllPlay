/**
 * @file   phone_crypto.h
 * @brief  手机侧（controller）配对密码学：SRP 客户端 + TLV 常量 + 密钥生成
 * @author blt
 */

// ============================================================================
// tools/phone_crypto.h —— 假手机侧的密码学（m3_test 与 fake_phone 共用）
// ============================================================================
// 这里放的是"站在 iPhone 那一侧"才需要的东西，与 core/airplay/session.cpp 的
// 服务端实现**对称**。之所以让自测（m3_test）和端到端工具（fake_phone）共用同一份：
//   · 分成两份，SRP 的 N/g/k 或者 PIN 一旦改动就要改两处，漏一处就是"端到端失败但
//     自测全绿"，最难查的那种。
//   · 两份实现还会掩盖协议理解的错误 —— 它们会一起错。
//
// 只依赖 core 的 crypto + OpenSSL，不依赖 ffmpeg/SDL。
#pragma once

#include "crypto/chacha20poly1305.h"
#include "crypto/ed25519.h"
#include "crypto/hkdf.h"   // sha512() 也在这里
#include "crypto/tlv8.h"
#include "crypto/x25519.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace allplay::phone {

using core::crypto::Tlv;

// TLV8 的 type 号（与 core/airplay/session.cpp 的匿名命名空间里那组一致）
constexpr uint8_t T_STATE = 0x06, T_PUBLIC_KEY = 0x03, T_SALT = 0x02, T_PROOF = 0x04;
constexpr uint8_t T_ENCRYPTED_DATA = 0x05, T_IDENTIFIER = 0x01, T_SIGNATURE = 0x0a;

inline std::vector<uint8_t> sb(const char* s)
{
    return std::vector<uint8_t>(s, s + std::strlen(s));
}

inline std::vector<uint8_t> hex(const char* s)
{
    std::vector<uint8_t> out;
    const size_t n = std::strlen(s);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    out.reserve(n / 2);
    for (size_t i = 0; i + 1 < n; i += 2)
        out.push_back(uint8_t((nib(s[i]) << 4) | nib(s[i + 1])));
    return out;
}

/// SRP-6a 的 3072 位模数 N（lowercase hex）。协议固定值，不是谁生成的。
inline const char* srpNHex()
{
    return "ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74"
           "020bbea63b139b22514a08798e3404ddef9519b3cd3a431b302b0a6df25f1437"
           "4fe1356d6d51c245e485b576625e7ec6f44c42e9a637ed6b0bff5cb6f406b7ed"
           "ee386bfb5a899fa5ae9f24117c4b1fe649286651ece45b3dc2007cb8a163bf05"
           "98da48361c55d39a69163fa8fd24cf5f83655d23dca3ad961c62f356208552bb"
           "9ed529077096966d670c354e4abc9804f1746c08ca18217c32905e462e36ce3b"
           "e39e772c180e86039b2783a2ec07a28fb5c55df06f4c52c9de2bcbf695581718"
           "3995497cea956ae515d2261898fa051015728e5a8aaac42dad33170d04507a33"
           "a85521abdf1cba64ecfb850458dbef0a8aea71575d060c7db3970f85a6e1e4c7"
           "abf5ae8cdb0933d71e8c94e04a25619dcee3d2261ad2ee6bf12ffa06d98a0864"
           "d87602733ec86a64521f2b18177b200cbbe117577a615d6c770988c0bad946e2"
           "08e24fa074e5ab3143db5bfce0fd108e4b82d120a93ad2caffffffffffffffff";
}

/// 生成一对 Ed25519 长期密钥；公钥写进 pub[32]。返回的 EVP_PKEY 由调用方释放。
inline EVP_PKEY* genEd25519(uint8_t pub[32])
{
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    EVP_PKEY* k = nullptr;
    if (ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &k) > 0 && k) {
        size_t plen = 32;
        EVP_PKEY_get_raw_public_key(k, pub, &plen);
    }
    if (ctx) EVP_PKEY_CTX_free(ctx);
    return k;
}

/// 手机侧 SRP 客户端：算出 A/M1/K，验 M2。
/// 对应服务端的 core::crypto::Srp6aServer（core/crypto/srp6a.h）。
struct SrpPhoneSide {
    BIGNUM *n = nullptr, *g = nullptr, *k = nullptr, *a = nullptr, *A = nullptr;
    std::vector<uint8_t> Apad, M1, K;
    std::string pin = "3939";

    explicit SrpPhoneSide(const std::string& pinCode = "3939") : pin(pinCode)
    {
        const auto nb = hex(srpNHex());
        n = BN_bin2bn(nb.data(), int(nb.size()), nullptr);
        g = BN_new();
        BN_set_word(g, 5);
        std::vector<uint8_t> pg(384, 0);
        pg[383] = 5;                                   // g 补齐到 N 的长度
        std::vector<uint8_t> in = nb;
        in.insert(in.end(), pg.begin(), pg.end());
        auto kh = core::crypto::sha512(in);
        k = BN_bin2bn(kh.data(), int(kh.size()), nullptr);
        uint8_t ab[32];
        RAND_bytes(ab, 32);
        a = BN_bin2bn(ab, 32, nullptr);
        A = BN_new();
        BN_CTX* ctx = BN_CTX_new();
        BN_mod_exp(A, g, a, n, ctx);
        BN_CTX_free(ctx);
        const int al = BN_num_bytes(A);
        Apad.assign(384, 0);
        BN_bn2bin(A, Apad.data() + (384 - al));
    }
    ~SrpPhoneSide()
    {
        BN_free(n); BN_free(g); BN_free(k); BN_free(a); BN_free(A);
    }
    SrpPhoneSide(const SrpPhoneSide&) = delete;
    SrpPhoneSide& operator=(const SrpPhoneSide&) = delete;

    /// 用服务端 M2 里的 salt/B 算出证明 M1，同时留下 K 供后续派生会话密钥。
    bool prove(const std::vector<uint8_t>& salt, const std::vector<uint8_t>& Bpad,
               std::vector<uint8_t>* m1out)
    {
        // x = SHA512(salt ‖ SHA512("Pair-Setup:" + PIN))；PIN 参与，所以 PIN 错就过不了
        auto inner = core::crypto::sha512(sb(("Pair-Setup:" + pin).c_str()));
        std::vector<uint8_t> xi = salt;
        xi.insert(xi.end(), inner.begin(), inner.end());
        BIGNUM* x = BN_bin2bn(core::crypto::sha512(xi).data(), 64, nullptr);

        std::vector<uint8_t> ui = Apad;
        ui.insert(ui.end(), Bpad.begin(), Bpad.end());
        BIGNUM* u = BN_bin2bn(core::crypto::sha512(ui).data(), 64, nullptr);

        BN_CTX* ctx = BN_CTX_new();
        BIGNUM* B = BN_bin2bn(Bpad.data(), int(Bpad.size()), nullptr);
        BIGNUM* gx = BN_new();   BN_mod_exp(gx, g, x, n, ctx);
        BIGNUM* kgx = BN_new();  BN_mod_mul(kgx, k, gx, n, ctx);
        BIGNUM* base = BN_new(); BN_mod_sub(base, B, kgx, n, ctx);   // B - k*g^x
        BIGNUM* ux = BN_new();   BN_mod_mul(ux, u, x, n, ctx);
        BIGNUM* ex = BN_new();   BN_mod_add(ex, a, ux, n, ctx);      // a + u*x
        BIGNUM* S = BN_new();    BN_mod_exp(S, base, ex, n, ctx);    // (B-kg^x)^(a+ux)
        const int sl = BN_num_bytes(S);
        std::vector<uint8_t> sbytes(static_cast<size_t>(sl));
        BN_bn2bin(S, sbytes.data());
        K = core::crypto::sha512(sbytes);

        auto hn = core::crypto::sha512(hex(srpNHex()));
        auto hg = core::crypto::sha512(std::vector<uint8_t>{5});
        std::vector<uint8_t> hx(64);
        for (int i = 0; i < 64; ++i) hx[i] = uint8_t(hn[i] ^ hg[i]);
        auto hu = core::crypto::sha512(sb("Pair-Setup"));
        std::vector<uint8_t> m1in = hx;
        m1in.insert(m1in.end(), hu.begin(), hu.end());
        m1in.insert(m1in.end(), salt.begin(), salt.end());
        m1in.insert(m1in.end(), Apad.begin(), Apad.end());
        m1in.insert(m1in.end(), Bpad.begin(), Bpad.end());
        m1in.insert(m1in.end(), K.begin(), K.end());
        M1 = core::crypto::sha512(m1in);
        *m1out = M1;

        BN_free(x); BN_free(u); BN_free(B); BN_free(gx); BN_free(kgx);
        BN_free(base); BN_free(ux); BN_free(ex); BN_free(S);
        BN_CTX_free(ctx);
        return true;
    }

    /// M2 = SHA512(A ‖ M1 ‖ K)（服务端证明它也算出了同一个 K）
    bool checkM2(const std::vector<uint8_t>& m2)
    {
        std::vector<uint8_t> m2in = Apad;
        m2in.insert(m2in.end(), M1.begin(), M1.end());
        m2in.insert(m2in.end(), K.begin(), K.end());
        return core::crypto::sha512(m2in) == m2;
    }
};

}  // namespace allplay::phone
