// ============================================================================
// tools/m3_test.cpp —— M3 加密层自测（不插硬件能验的最高程度）
// ============================================================================
// 覆盖：
//   ① TLV8：编解码往返、>255 分片、同 type 分隔、手写字节解码
//   ② ChaCha20-Poly1305：加解密往返、篡改 tag 失败、nonce 形态
//   ③ HKDF-SHA512：确定性、输出长度
//   ④ X25519：双方共享密钥一致
//   ⑤ Ed25519：签/验往返、错签名失败
//   ⑥ SRP-6a：固定盐/指数对照 Python 独立计算的参考向量（B/K/M1/M2），
//      以及 A%N==0、篡改 M1 的阴性对照
//
// 用法： m3_test
#include "airplay/control_cipher.h"
#include "airplay/session.h"
#include "airplay/video_stream.h"
#include "crypto/chacha20poly1305.h"
#include "crypto/ed25519.h"
#include "crypto/hkdf.h"
#include "crypto/srp6a.h"
#include "crypto/tlv8.h"
#include "crypto/x25519.h"
#include "platform/device_identity.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) { std::printf("   ✅ %s\n", msg); }                       \
        else      { std::printf("   ❌ %s\n", msg); ++g_fail; }             \
    } while (0)

std::vector<uint8_t> hex(const char* s)
{
    std::vector<uint8_t> out;
    size_t n = std::strlen(s);
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

// ---------------------------------------------------------------------------
// ① TLV8
// ---------------------------------------------------------------------------
void testTlv8()
{
    std::printf("== ① TLV8 ==\n");
    using namespace core::crypto;

    std::vector<Tlv> items = {
        {0x06, {2}},
        {0x03, std::vector<uint8_t>(64, 0xAB)},
        {0x05, std::vector<uint8_t>(300, 0xCD)},
    };
    auto enc = tlv8Encode(items);
    auto dec = tlv8Decode(enc.data(), enc.size());
    const auto* state = tlv8Get(dec, 0x06);
    const auto* pub   = tlv8Get(dec, 0x03);
    const auto* encd  = tlv8Get(dec, 0x05);
    CHECK(state && state->size() == 1 && (*state)[0] == 2, "STATE 单字节往返");
    CHECK(pub && pub->size() == 64 && pub->front() == 0xAB, "64 字节 value 往返");
    CHECK(encd && encd->size() == 300 && encd->back() == 0xCD, "300 字节分片拼接");

    // >255 分片：300 字节应切 255+45，两片都带 [type][len] 头
    // 找到 0x05 片：enc 里 type 0x05 出现两次，len 分别是 255 和 45
    int frag255 = 0, frag45 = 0;
    for (size_t i = 0; i + 1 < enc.size();) {
        uint8_t t = enc[i], l = enc[i + 1];
        if (t == 0x05) { if (l == 255) ++frag255; else if (l == 45) ++frag45; }
        i += 2 + l;
    }
    CHECK(frag255 == 1 && frag45 == 1, "300 字节切成 255+45 两片");

    // 同 type 两条条目 → 分隔 (0xff,0x00)
    std::vector<Tlv> dup = {{0x06, {1}}, {0x06, {9}}};
    auto enc2 = tlv8Encode(dup);
    bool hasSep = false;
    for (size_t i = 0; i + 1 < enc2.size(); ++i)
        if (enc2[i] == 0xFF && enc2[i + 1] == 0x00) { hasSep = true; break; }
    CHECK(hasSep, "相邻同 type 条目间有 0xff/0x00 分隔");
    auto dec2 = tlv8Decode(enc2.data(), enc2.size());
    const auto* v6 = tlv8Get(dec2, 0x06);
    CHECK(v6 && v6->size() == 1 && (*v6)[0] == 9, "同 type 后一条覆盖前一条");
}

// ---------------------------------------------------------------------------
// ② ChaCha20-Poly1305
// ---------------------------------------------------------------------------
void testChaCha()
{
    std::printf("== ② ChaCha20-Poly1305 ==\n");
    using namespace core::crypto;

    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = uint8_t(i);
    const std::vector<uint8_t> pt = {'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    const std::vector<uint8_t> aad = {0x01, 0x02, 0x03};

    auto n = nonce64(0);
    auto sealed = chachaSeal(key, n, aad.data(), aad.size(), pt.data(), pt.size());
    CHECK(sealed.size() == pt.size() + 16, "密文 = 明文 + 16 字节 tag");

    std::vector<uint8_t> back;
    CHECK(chachaOpen(key, n, aad.data(), aad.size(), sealed.data(), sealed.size(), &back) &&
          back == pt, "加解密往返一致");

    // 篡改 tag → 失败
    auto bad = sealed;
    bad.back() ^= 0xFF;
    CHECK(!chachaOpen(key, n, aad.data(), aad.size(), bad.data(), bad.size(), &back),
          "篡改 tag 解密失败（阴性）");

    // 篡改密文 → 失败
    bad = sealed;
    bad[0] ^= 0x01;
    CHECK(!chachaOpen(key, n, aad.data(), aad.size(), bad.data(), bad.size(), &back),
          "篡改密文解密失败（阴性）");

    // nonce 形态
    auto n1 = nonce64(1);
    CHECK(n1[0] == 0 && n1[1] == 0 && n1[2] == 0 && n1[3] == 0 &&
          n1[4] == 1 && n1[8] == 0, "nonce64(1) = 4 零字节 + LE 计数器");

    auto nl = nonceLabel("PV-Msg02");
    CHECK(nl[4] == 'P' && nl[5] == 'V' && nl[8] == 's' && nl[11] == '2',
          "nonceLabel(\"PV-Msg02\") 形态");
}

// ---------------------------------------------------------------------------
// ③ HKDF-SHA512
// ---------------------------------------------------------------------------
void testHkdf()
{
    std::printf("== ③ HKDF-SHA512 ==\n");
    using namespace core::crypto;

    std::vector<uint8_t> ikm = {0x00, 0x01, 0x02, 0x03};
    auto k1 = hkdfSha512(ikm, "Salt", "Info", 32);
    auto k2 = hkdfSha512(ikm, "Salt", "Info", 32);
    auto k3 = hkdfSha512(ikm, "Salt", "Other", 32);
    auto k64 = hkdfSha512(ikm, "Salt", "Info", 64);
    CHECK(k1.size() == 32 && k1 == k2, "确定性 + 32 字节输出");
    CHECK(k1 != k3, "不同 info → 不同密钥");
    CHECK(k64.size() == 64, "64 字节输出");
}

// ---------------------------------------------------------------------------
// ④ X25519
// ---------------------------------------------------------------------------
void testX25519()
{
    std::printf("== ④ X25519 ==\n");
    using namespace core::crypto;

    auto a = x25519Generate();
    auto b = x25519Generate();
    auto sa = x25519Shared(a.priv.data(), b.pub.data());
    auto sb = x25519Shared(b.priv.data(), a.pub.data());
    CHECK(!sa.empty() && sa == sb, "双方共享密钥一致（32 字节）");
}

// ---------------------------------------------------------------------------
// ⑤ Ed25519
// ---------------------------------------------------------------------------
void testEd25519()
{
    std::printf("== ⑤ Ed25519 ==\n");
    using namespace core::crypto;

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    EVP_PKEY* key = nullptr;
    bool okgen = ctx && EVP_PKEY_keygen_init(ctx) > 0 && EVP_PKEY_keygen(ctx, &key) > 0;
    CHECK(okgen, "生成 Ed25519 密钥");
    if (!okgen) { if (ctx) EVP_PKEY_CTX_free(ctx); return; }

    uint8_t pub[32]; size_t plen = 32;
    EVP_PKEY_get_raw_public_key(key, pub, &plen);

    const uint8_t msg[7] = {'p', 'a', 'i', 'r', 'v', 'e', 'r'};
    auto sig = ed25519Sign(key, msg, sizeof msg);
    CHECK(sig.size() == 64, "签名 = 64 字节");
    CHECK(ed25519Verify(pub, msg, sizeof msg, sig.data()), "验签通过");
    uint8_t bad[7]; std::memcpy(bad, msg, 7); bad[0] ^= 1;
    CHECK(!ed25519Verify(pub, bad, sizeof bad, sig.data()), "错消息验签失败（阴性）");

    EVP_PKEY_free(key);
    EVP_PKEY_CTX_free(ctx);
}

// ---------------------------------------------------------------------------
// ⑥ SRP-6a（对照 Python 独立计算的参考向量）
// ---------------------------------------------------------------------------
void testSrp()
{
    std::printf("== ⑥ SRP-6a ==\n");
    using namespace core::crypto;

    // 固定 salt(00..0f) + b(01..20)；Python 独立计算出的参考向量
    const std::vector<uint8_t> salt = hex("000102030405060708090a0b0c0d0e0f");
    const std::vector<uint8_t> b = hex(
        "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
    const std::vector<uint8_t> A = hex(
        "352dd9b14781c00e07d64153a8c8d095cbb05ec357d048100af82fcc5b6a5b57"
        "9b701db5545cb0aecb51fee6bd68cd66de988f4ff86c9b477594564150d00fba"
        "526d7b8dc106489618e404e4393beaffe3855d5a0c530ad39b1b80de5b31612f"
        "724e4caa49654436336976cd5e52b7f4aa5bf376115e6b142967cca7357c95cd"
        "08e6fa0c86e3118dab9732db62beb729084256af7759d4a0cf4fc2bfbaa04e7e"
        "79b9dc1e5790d43296faebc78407c4d7a8bad16df2f64d60b6f8e0b05eb07197"
        "03a7e2dd562d2f12330a86dac49c3dd84830a273c880858dc2de20b7b554b4a8"
        "10666a027a1c105764f1695aae5544b4e4acd950409fb4233e5dfaa1cb51becb"
        "9b4ffc79dafc9d022d162fda4913f642537b4dc67b9970530997fed9d2015a24"
        "318aae5896b2b1f684d09d7577bb43549caeaa96a56d5ca99e43a4841e4f2b0d"
        "3f2a8d3aa8e45b4113b4603cbfe272ef28f38c85e3a3108d93876c0076c1cf40"
        "3f38f462b1884f2371067d537584ee9907034638dab0e6be43bb30f170060db8");
    const std::vector<uint8_t> Bref = hex(
        "73a722136dd3ef4c25cab9627da94b06f71057181457c774d77485e790fe9793"
        "a0f12ac46fadb2a6ba38f9452de3c4c0fdbdcf927476affc2cb7a3849bb20a97"
        "4c69ca9e70d7d78b7a3d8535b75bbafa47559addb49548113279fbdb0b385272"
        "a2f4d0acf85689370ccc0ac1746e057a13e3f725d2a5ce1bc5857c49f9b982fe"
        "f36c3b19da4c14be368ec00a7f2140919f8163d372428a119a125eddfa9f4cc8"
        "74681f1e6f2a3debf91610ebe493e85435845fdf623246e6e79b79942fc855f8"
        "b549b19145c583cbbe38518d01f0031e9c4612d3987562aa3a9230d881a4d361"
        "32ff948d459d00c8cfef259a925caa4ae10617c7a90aee895bd282cce8bb24b5"
        "b6e8b4d68b21360754789de6f56c4abf4a709b8d84db579fa6685899dad3511d"
        "99a8c005e53c72e58af0eb92e85a879a22132936039a3d5e6cbba4a52febdf90"
        "382f455caed759596985822984eb4257d2511540621e7d080bf4cfa4df2e6545"
        "94d0fd2cf258550e1810eba6220e67f1c66618f9c62becb4eb2335a116890cac");
    const std::vector<uint8_t> M1 = hex(
        "3bcd5542bbe26311daf1e9622e101687cb991e406ed216c71975bd62bafde550"
        "f3a8f930e3b4040e6bdba938f9b916890f66d9754f9d16b2936fc2f34cc4d44a");
    const std::vector<uint8_t> Kref = hex(
        "1cc3a02b04edc6c7f28c6dd6ac73bc1ad1dcada43eacab30a55615925a78c83c"
        "9581959e4482f957315ebd73c53f0f750a42ad5b42b8f76a69869a17a7648333");
    const std::vector<uint8_t> M2ref = hex(
        "3bca3cea127ca8ce31cd8bf3584af063a5f28a5e16a9cda7fc8a0f9e0a6b5be3"
        "956372be1ba6c81472edc29075eafddbba7c1f4bf67808aec6adf3349fbdc09e");

    Srp6aServer srv(salt.data(), b.data());
    CHECK(srv.salt() == salt, "固定盐 16 字节");
    CHECK(srv.publicKey() == Bref, "B = (k*v + g^b) mod N（384 字节，对参考向量）");

    std::vector<uint8_t> K, M2;
    bool ok = srv.verify(A.data(), A.size(), M1.data(), M1.size(), &K, &M2);
    CHECK(ok, "verify(A, M1) 通过");
    CHECK(K == Kref, "会话密钥 K 对参考向量");
    CHECK(M2 == M2ref, "服务端 proof M2 对参考向量");

    // 篡改 M1 → 失败
    std::vector<uint8_t> m1bad = M1; m1bad[0] ^= 1;
    CHECK(!srv.verify(A.data(), A.size(), m1bad.data(), m1bad.size(), &K, &M2),
          "篡改 M1 验证失败（阴性）");

    // A = N（A mod N == 0）→ 拒绝
    const std::vector<uint8_t> Nhex = hex(
        "ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74"
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
        "08e24fa074e5ab3143db5bfce0fd108e4b82d120a93ad2caffffffffffffffff");
    CHECK(!srv.verify(Nhex.data(), Nhex.size(), M1.data(), M1.size(), &K, &M2),
          "A mod N == 0 被拒绝（阴性）");
}

// ---------------------------------------------------------------------------
// ⑦ ControlCipher 帧格式往返
// ---------------------------------------------------------------------------
void testControlCipher()
{
    std::printf("== ⑦ ControlCipher 帧格式 ==\n");
    using core::airplay::ControlCipher;

    uint8_t rk[32], wk[32];
    for (int i = 0; i < 32; ++i) { rk[i] = uint8_t(i); wk[i] = uint8_t(0x80 + i); }
    ControlCipher enc(rk, wk);   // 写侧：writeKey=wk
    ControlCipher dec(wk, rk);   // 读侧：readKey=wk（与 enc 的 writeKey 交叉）

    // 短消息
    const std::vector<uint8_t> msg = {'a', 'b', 'c'};
    auto framed = enc.encrypt(msg.data(), msg.size());
    CHECK(framed.size() == 2 + 3 + 16, "帧 = 2B 头 + 密文 + 16B tag");
    CHECK(framed[0] == 3 && framed[1] == 0, "长度头 = 小端 3");

    dec.feed(framed.data(), framed.size());
    std::vector<uint8_t> out;
    CHECK(dec.next(&out) && out == msg, "加密→喂入→解密 一致");
    CHECK(!dec.next(&out), "无残留帧");

    // 大消息分帧（>0x4000）；用全新写侧（计数器从 0 起，与 dec2 对齐）
    std::vector<uint8_t> big(0x4000 + 100, 0x5A);
    ControlCipher enc2(rk, wk);
    auto framed2 = enc2.encrypt(big.data(), big.size());
    ControlCipher dec2(wk, rk);
    dec2.feed(framed2.data(), framed2.size());
    std::vector<uint8_t> merged;
    std::vector<uint8_t> part;
    while (dec2.next(&part)) { merged.insert(merged.end(), part.begin(), part.end()); }
    CHECK(merged == big, ">0x4000 分帧后拼回一致");
}

// ---------------------------------------------------------------------------
// ⑧ 会话流程：假手机走 pair-setup → pair-verify → 控制密钥
// ---------------------------------------------------------------------------
namespace {

using core::crypto::Tlv;

constexpr uint8_t T_STATE = 0x06, T_PUBLIC_KEY = 0x03, T_SALT = 0x02, T_PROOF = 0x04;
constexpr uint8_t T_ENCRYPTED_DATA = 0x05, T_IDENTIFIER = 0x01, T_SIGNATURE = 0x0a;

// 与 testSrp 里同一个 N（lowercase）。
const char* kNHexTest =
    "ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74"
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

std::vector<uint8_t> sb(const char* s) { return std::vector<uint8_t>(s, s + std::strlen(s)); }

EVP_PKEY* genEd25519(uint8_t pub[32])
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

/// 手机侧 SRP 客户端（算 A/M1/K，验 M2）。
struct SrpPhoneSide {
    BIGNUM *n, *g, *k, *a, *A;
    std::vector<uint8_t> Apad, M1, K;

    SrpPhoneSide()
    {
        const auto nb = hex(kNHexTest);
        n = BN_bin2bn(nb.data(), int(nb.size()), nullptr);
        g = BN_new(); BN_set_word(g, 5);
        std::vector<uint8_t> pg(384, 0); pg[383] = 5;
        std::vector<uint8_t> in = nb; in.insert(in.end(), pg.begin(), pg.end());
        auto kh = core::crypto::sha512(in);
        k = BN_bin2bn(kh.data(), int(kh.size()), nullptr);
        uint8_t ab[32]; RAND_bytes(ab, 32);
        a = BN_bin2bn(ab, 32, nullptr);
        A = BN_new();
        BN_CTX* ctx = BN_CTX_new();
        BN_mod_exp(A, g, a, n, ctx);
        BN_CTX_free(ctx);
        int al = BN_num_bytes(A);
        Apad.assign(384, 0);
        BN_bn2bin(A, Apad.data() + (384 - al));
    }
    ~SrpPhoneSide() { BN_free(n); BN_free(g); BN_free(k); BN_free(a); BN_free(A); }

    bool prove(const std::vector<uint8_t>& salt, const std::vector<uint8_t>& Bpad,
               std::vector<uint8_t>* m1out)
    {
        auto inner = core::crypto::sha512(sb("Pair-Setup:3939"));
        std::vector<uint8_t> xi = salt; xi.insert(xi.end(), inner.begin(), inner.end());
        BIGNUM* x = BN_bin2bn(core::crypto::sha512(xi).data(), 64, nullptr);

        std::vector<uint8_t> ui = Apad; ui.insert(ui.end(), Bpad.begin(), Bpad.end());
        BIGNUM* u = BN_bin2bn(core::crypto::sha512(ui).data(), 64, nullptr);

        BN_CTX* ctx = BN_CTX_new();
        BIGNUM* B = BN_bin2bn(Bpad.data(), int(Bpad.size()), nullptr);
        BIGNUM* gx = BN_new(); BN_mod_exp(gx, g, x, n, ctx);
        BIGNUM* kgx = BN_new(); BN_mod_mul(kgx, k, gx, n, ctx);
        BIGNUM* base = BN_new(); BN_mod_sub(base, B, kgx, n, ctx);
        BIGNUM* ux = BN_new(); BN_mod_mul(ux, u, x, n, ctx);
        BIGNUM* ex = BN_new(); BN_mod_add(ex, a, ux, n, ctx);
        BIGNUM* S = BN_new(); BN_mod_exp(S, base, ex, n, ctx);
        int sl = BN_num_bytes(S);
        std::vector<uint8_t> sbytes(static_cast<size_t>(sl)); BN_bn2bin(S, sbytes.data());
        K = core::crypto::sha512(sbytes);

        auto hn = core::crypto::sha512(hex(kNHexTest));
        auto hg = core::crypto::sha512(std::vector<uint8_t>{5});
        std::vector<uint8_t> hx(64);
        for (int i = 0; i < 64; ++i) hx[i] = hn[i] ^ hg[i];
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

    bool checkM2(const std::vector<uint8_t>& m2)
    {
        std::vector<uint8_t> m2in = Apad;
        m2in.insert(m2in.end(), M1.begin(), M1.end());
        m2in.insert(m2in.end(), K.begin(), K.end());
        return core::crypto::sha512(m2in) == m2;
    }
};

}  // namespace

void testSessionFlow()
{
    std::printf("== ⑧ 会话流程（假手机 pair-setup → pair-verify）==\n");
    using namespace core::crypto;

    // 本机身份 + 会话
    core::DeviceIdentity ident;
    std::string err;
    CHECK(ident.loadOrCreate("/tmp/allplay-m3-identity", &err), "生成本机 Ed25519 身份");

    core::airplay::DeviceInfo info;
    info.name = "AllPlay"; info.deviceId = "AA:BB:CC:DD:EE:FF";
    info.pi = ident.pi; info.pkHex = ident.pkHex;

    core::airplay::SessionConfig sc;
    sc.identity = &ident; sc.info = &info;
    sc.stateDir = "/tmp/allplay-m3-pairing";
    sc.mfiDir.clear();   // 本轮跳过 auth-setup（MFi 签名在 iap2_test 已验）
    core::airplay::Session session(sc);

    const std::string controllerId = "11111111-2222-3333-4444-555555555555";
    uint8_t cpub[32];
    EVP_PKEY* cpriv = genEd25519(cpub);

    // ---- pair-setup M1 → M2 ----
    {
        core::airplay::RtspRequest r; r.method = "POST"; r.uri = "/pair-setup";
        r.body = tlv8Encode({{T_STATE, {1}}});
        auto resp = session.handle(r);
        auto items = tlv8Decode(resp.body.data(), resp.body.size());
        const auto* st = tlv8Get(items, T_STATE);
        const auto* B = tlv8Get(items, T_PUBLIC_KEY);
        const auto* salt = tlv8Get(items, T_SALT);
        CHECK(st && st->front() == 2 && B && B->size() == 384 && salt && salt->size() == 16,
              "M2 = STATE 2 + B(384) + salt(16)");
        CHECK(resp.header("Content-Type") == "application/pairing+tlv8", "pair-setup Content-Type");

        // M3
        SrpPhoneSide phone;
        std::vector<uint8_t> M1;
        phone.prove(*salt, *B, &M1);
        std::vector<Tlv> m3 = {{T_STATE, {3}}, {T_PUBLIC_KEY, phone.Apad}, {T_PROOF, M1}};
        r.body = tlv8Encode(m3);
        auto resp4 = session.handle(r);
        auto it4 = tlv8Decode(resp4.body.data(), resp4.body.size());
        const auto* st4 = tlv8Get(it4, T_STATE);
        const auto* M2 = tlv8Get(it4, T_PROOF);
        CHECK(st4 && st4->front() == 4 && M2 && phone.checkM2(*M2), "M4 = STATE 4 + 正确 M2");

        // M5（controller 长期密钥 + 签名）
        std::vector<uint8_t> cSignKey = hkdfSha512(phone.K, "Pair-Setup-Controller-Sign-Salt",
                                                   "Pair-Setup-Controller-Sign-Info", 32);
        std::vector<uint8_t> signData = cSignKey;
        signData.insert(signData.end(), controllerId.begin(), controllerId.end());
        signData.insert(signData.end(), cpub, cpub + 32);
        auto cSig = ed25519Sign(cpriv, signData.data(), signData.size());

        std::vector<Tlv> inner = {{T_IDENTIFIER, sb(controllerId.c_str())},
                                  {T_PUBLIC_KEY, std::vector<uint8_t>(cpub, cpub + 32)},
                                  {T_SIGNATURE, cSig}};
        auto encKey = hkdfSha512(phone.K, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", 32);
        auto innerBytes = tlv8Encode(inner);
        auto sealed = chachaSeal(encKey.data(), nonceLabel("PS-Msg05"), nullptr, 0,
                                 innerBytes.data(), innerBytes.size());
        std::vector<Tlv> m5 = {{T_STATE, {5}}, {T_ENCRYPTED_DATA, sealed}};
        r.body = tlv8Encode(m5);
        auto resp6 = session.handle(r);
        auto it6 = tlv8Decode(resp6.body.data(), resp6.body.size());
        const auto* st6 = tlv8Get(it6, T_STATE);
        const auto* enc6 = tlv8Get(it6, T_ENCRYPTED_DATA);
        bool m6ok = st6 && st6->front() == 6 && enc6;
        std::vector<uint8_t> accInnerBytes;
        if (enc6) {
            m6ok = m6ok && chachaOpen(encKey.data(), nonceLabel("PS-Msg06"), nullptr, 0,
                                      enc6->data(), enc6->size(), &accInnerBytes);
        }
        CHECK(m6ok, "M6 = STATE 6 + 可解密 ENCRYPTED_DATA");
        if (m6ok) {
            auto accInner = tlv8Decode(accInnerBytes.data(), accInnerBytes.size());
            const auto* aid = tlv8Get(accInner, T_IDENTIFIER);
            const auto* apk = tlv8Get(accInner, T_PUBLIC_KEY);
            const auto* asg = tlv8Get(accInner, T_SIGNATURE);
            auto aSignKey = hkdfSha512(phone.K, "Pair-Setup-Accessory-Sign-Salt",
                                       "Pair-Setup-Accessory-Sign-Info", 32);
            std::vector<uint8_t> aData = aSignKey;
            aData.insert(aData.end(), aid->begin(), aid->end());
            aData.insert(aData.end(), apk->begin(), apk->end());
            CHECK(aid && apk && asg && asg->size() == 64 &&
                  *aid == sb(ident.pi.c_str()) && *apk == ident.pub &&
                  ed25519Verify(apk->data(), aData.data(), aData.size(), asg->data()),
                  "M6 内层 = 本机 identifier + 公钥 + 可验签名");
        }
    }

    // ---- pair-verify ----
    {
        auto clientEph = x25519Generate();
        core::airplay::RtspRequest r; r.method = "POST"; r.uri = "/pair-verify";
        r.body = tlv8Encode({{T_STATE, {1}},
                             {T_PUBLIC_KEY, std::vector<uint8_t>(clientEph.pub.begin(), clientEph.pub.end())}});
        auto resp = session.handle(r);
        auto items = tlv8Decode(resp.body.data(), resp.body.size());
        const auto* st = tlv8Get(items, T_STATE);
        const auto* apub = tlv8Get(items, T_PUBLIC_KEY);
        const auto* enc = tlv8Get(items, T_ENCRYPTED_DATA);
        CHECK(st && st->front() == 2 && apub && apub->size() == 32 && enc,
              "PV-M2 = STATE 2 + 临时公钥 + 加密数据");

        auto shared = x25519Shared(clientEph.priv.data(), apub->data());
        auto pvKey = hkdfSha512(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", 32);
        std::vector<uint8_t> accInnerBytes;
        bool ok = chachaOpen(pvKey.data(), nonceLabel("PV-Msg02"), nullptr, 0,
                             enc->data(), enc->size(), &accInnerBytes);
        auto accInner = tlv8Decode(accInnerBytes.data(), accInnerBytes.size());
        const auto* aid = tlv8Get(accInner, T_IDENTIFIER);
        const auto* asg = tlv8Get(accInner, T_SIGNATURE);
        std::vector<uint8_t> sigData(apub->begin(), apub->end());
        sigData.insert(sigData.end(), ident.pi.begin(), ident.pi.end());
        sigData.insert(sigData.end(), clientEph.pub.begin(), clientEph.pub.end());
        CHECK(ok && aid && asg && *aid == sb(ident.pi.c_str()) &&
              ed25519Verify(ident.pub.data(), sigData.data(), sigData.size(), asg->data()),
              "PV-M2 内层签名可验（本机临时公钥 ‖ pi ‖ 客户端临时公钥）");

        // M3（controller 签名）
        std::vector<uint8_t> sigData2(clientEph.pub.begin(), clientEph.pub.end());
        sigData2.insert(sigData2.end(), controllerId.begin(), controllerId.end());
        sigData2.insert(sigData2.end(), apub->begin(), apub->end());
        auto cSig2 = ed25519Sign(cpriv, sigData2.data(), sigData2.size());
        std::vector<Tlv> inner2 = {{T_IDENTIFIER, sb(controllerId.c_str())}, {T_SIGNATURE, cSig2}};
        auto innerBytes2 = tlv8Encode(inner2);
        auto sealed2 = chachaSeal(pvKey.data(), nonceLabel("PV-Msg03"), nullptr, 0,
                                  innerBytes2.data(), innerBytes2.size());
        r.body = tlv8Encode({{T_STATE, {3}}, {T_ENCRYPTED_DATA, sealed2}});
        auto resp4 = session.handle(r);
        auto it4 = tlv8Decode(resp4.body.data(), resp4.body.size());
        const auto* st4 = tlv8Get(it4, T_STATE);
        CHECK(st4 && st4->front() == 4, "PV-M4 = STATE 4");

        CHECK(session.controlKeysReady(), "pair-verify 后控制密钥就绪");
        auto ck = session.takeControlKeys();
        auto readKey = hkdfSha512(shared, "Control-Salt", "Control-Write-Encryption-Key", 32);
        auto writeKey = hkdfSha512(shared, "Control-Salt", "Control-Read-Encryption-Key", 32);
        CHECK(std::equal(ck.readKey.begin(), ck.readKey.end(), readKey.begin()) &&
              std::equal(ck.writeKey.begin(), ck.writeKey.end(), writeKey.begin()),
              "控制密钥 = HKDF(shared, Control-Salt, ...)（read/write 方向交叉）");
    }

    if (cpriv) EVP_PKEY_free(cpriv);
}

// ---------------------------------------------------------------------------
// ⑨ 屏幕流：配置帧检测 + 解密 + AVCC→AnnexB + 关键帧闸门
// ---------------------------------------------------------------------------
namespace {

struct MockVideo : public core::IVideoOutput {
    std::mutex mu;
    bool gotConfig = false;
    core::VideoCodec codec = core::VideoCodec::H264;
    std::vector<uint8_t> codecData;
    std::vector<std::vector<uint8_t>> frames;

    void configure(core::VideoCodec c, int, int, const std::vector<uint8_t>& d) override
    {
        std::lock_guard<std::mutex> lk(mu);
        gotConfig = true; codec = c; codecData = d;
    }
    bool feedAnnexB(const uint8_t* d, size_t n) override
    {
        std::lock_guard<std::mutex> lk(mu);
        frames.emplace_back(d, d + n);
        return true;
    }
    void reset() override {}
    void flush() override {}
};

struct MockStream : public core::ITcpStream {
    std::vector<uint8_t> buf;
    size_t off = 0;
    int read(uint8_t* b, size_t n, int) override
    {
        if (off >= buf.size()) return 0;   // 超时语义：没数据了
        size_t take = std::min(n, buf.size() - off);
        std::memcpy(b, buf.data() + off, take);
        off += take;
        return int(take);
    }
    bool writeAll(const uint8_t*, size_t) override { return true; }
    void close() override {}
};

struct MockListener : public core::ITcpListener {
    std::unique_ptr<MockStream> stream;
    bool start(const std::string&, uint16_t, std::string*) override { return true; }
    std::unique_ptr<core::ITcpStream> accept(int) override { return std::move(stream); }
    uint16_t port() const override { return 54321; }
    void stop() override {}
};

}  // namespace

void testScreenStream()
{
    std::printf("== ⑨ 屏幕流 ==\n");
    using namespace core::crypto;

    std::vector<uint8_t> shared(32, 0x33);
    const std::string scid = "123";
    auto key = hkdfSha512(shared, "DataStream-Salt" + scid, "DataStream-Output-Encryption-Key", 32);

    // 配置帧：body = "avcC" + 4 字节伪 codecData
    std::vector<uint8_t> codecData = {0x01, 0x64, 0x00, 0x1f};
    std::vector<uint8_t> cfgBody = {'a', 'v', 'c', 'C'};
    cfgBody.insert(cfgBody.end(), codecData.begin(), codecData.end());

    // 视频帧：AVCC 一个 IDR NALU（nal type 5）
    std::vector<uint8_t> avcc = {0x00, 0x00, 0x00, 0x02, 0x65, 0x88};
    std::vector<uint8_t> annexb = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};

    auto makeFrame = [&](uint8_t opcode, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> header(128, 0);
        uint32_t bl = uint32_t(body.size());
        header[0] = bl & 0xFF; header[1] = (bl >> 8) & 0xFF;
        header[2] = (bl >> 16) & 0xFF; header[3] = (bl >> 24) & 0xFF;
        header[4] = opcode;
        std::vector<uint8_t> out = header;
        out.insert(out.end(), body.begin(), body.end());
        return out;
    };

    // 加密视频帧 body
    auto header128 = [&](const std::vector<uint8_t>& body, uint8_t opcode) {
        std::vector<uint8_t> h(128, 0);
        uint32_t bl = uint32_t(body.size());
        h[0] = bl & 0xFF; h[1] = (bl >> 8) & 0xFF;
        h[2] = (bl >> 16) & 0xFF; h[3] = (bl >> 24) & 0xFF;
        h[4] = opcode;
        return h;
    };
    // 视频帧：header 的 bodyLen = 密文+tag 长度（6 + 16），AAD 用同一个 header
    auto encHeader = header128(std::vector<uint8_t>(avcc.size() + 16, 0), 0);
    auto sealedBody = chachaSeal(key.data(), nonce64(0), encHeader.data(), 128,
                                 avcc.data(), avcc.size());

    // 组装 mock 流：配置帧 + 加密视频帧
    auto mockStream = std::make_unique<MockStream>();
    {
        auto cfgFrame = makeFrame(1, cfgBody);
        auto vidHeader = encHeader;
        std::vector<uint8_t> vidFrame = vidHeader;
        vidFrame.insert(vidFrame.end(), sealedBody.begin(), sealedBody.end());
        mockStream->buf.insert(mockStream->buf.end(), cfgFrame.begin(), cfgFrame.end());
        mockStream->buf.insert(mockStream->buf.end(), vidFrame.begin(), vidFrame.end());
    }
    auto mockListener = std::make_unique<MockListener>();
    mockListener->stream = std::move(mockStream);

    MockVideo video;
    core::airplay::ScreenStream ss(std::move(mockListener), shared, scid, 1280, 720, &video);

    // 等屏幕流线程处理完两帧（锁只在读状态时持有，睡眠时释放）
    bool done = false;
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lk(video.mu);
            if (video.gotConfig && !video.frames.empty()) { done = true; break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(done, "屏幕流：收到配置帧 + 视频帧");

    {
        std::lock_guard<std::mutex> lk(video.mu);
        CHECK(video.gotConfig && video.codec == core::VideoCodec::H264 && video.codecData == codecData,
              "配置帧：识别 avcC + 4 字节 codecData");
        CHECK(video.frames.size() == 1 && video.frames[0] == annexb,
              "视频帧：解密后 AVCC→AnnexB 且关键帧闸门放行");
    }

    ss.stop();   // 不在锁内停止（避免与流线程互锁）
}

// ---------------------------------------------------------------------------
// ⑩ 音频：RTP 乱序重排 + 解密
// ---------------------------------------------------------------------------
namespace {

struct MockUdp : public core::IUdpSocket {
    std::mutex mu;
    std::deque<std::vector<uint8_t>> pkts;
    uint16_t p = 0;
    bool closed_ = false;

    bool bind(uint16_t, std::string*) override
    {
        static int c = 50000;
        p = uint16_t(++c);
        return true;
    }
    int recv(uint8_t* buf, size_t n, int) override
    {
        std::vector<uint8_t> v;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (closed_) return -1;
            if (!pkts.empty()) { v = std::move(pkts.front()); pkts.pop_front(); }
        }
        if (v.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); return 0; }
        const size_t take = std::min(n, v.size());
        std::memcpy(buf, v.data(), take);
        return int(take);
    }
    bool send(const uint8_t*, size_t) override { return true; }
    uint16_t port() const override { return p; }
    void close() override { std::lock_guard<std::mutex> lk(mu); closed_ = true; }
    void inject(const std::vector<uint8_t>& v) { std::lock_guard<std::mutex> lk(mu); pkts.push_back(v); }
};

struct MockAudioOut : public core::IAudioOutput {
    std::mutex mu;
    std::vector<uint32_t> samples;   // 按吐出顺序记录的 RTP timestamp
    void configure(core::AudioCodec, int, int) override {}
    void feed(const uint8_t*, size_t, uint32_t sample) override
    {
        std::lock_guard<std::mutex> lk(mu);
        samples.push_back(sample);
    }
    void reset() override {}
};

}  // namespace

void testAudioReorder()
{
    std::printf("== ⑩ 音频乱序重排 ==\n");
    using namespace core::crypto;

    std::vector<uint8_t> shared(32, 0x44);
    const std::string scid = "7";
    auto key = hkdfSha512(shared, "DataStream-Salt" + scid, "DataStream-Output-Encryption-Key", 32);

    // 组一个合法加密 RTP 包：[12B RTP 头][密文+16B tag][8B nonce 尾]
    auto rtp = [&](uint16_t seq, uint32_t ts) {
        std::vector<uint8_t> hdr(12, 0);
        hdr[2] = uint8_t(seq >> 8); hdr[3] = uint8_t(seq & 0xff);
        hdr[4] = uint8_t(ts >> 24); hdr[5] = uint8_t(ts >> 16);
        hdr[6] = uint8_t(ts >> 8);  hdr[7] = uint8_t(ts);
        std::vector<uint8_t> payload(20, uint8_t(seq));   // 载荷用 seq 填充
        std::array<uint8_t, 8> tail{};
        tail[0] = uint8_t(seq); tail[1] = uint8_t(seq >> 8);   // nonce 尾取 seq，保证逐包不同
        std::array<uint8_t, 12> nonce{};
        std::memcpy(nonce.data() + 4, tail.data(), 8);
        auto sealed = chachaSeal(key.data(), nonce, hdr.data() + 4, 8,
                                 payload.data(), payload.size());
        std::vector<uint8_t> out = hdr;
        out.insert(out.end(), sealed.begin(), sealed.end());   // 密文 + tag
        out.insert(out.end(), tail.begin(), tail.end());       // 8B nonce 尾
        return out;
    };

    MockUdp* dataSock = new MockUdp;
    MockUdp* ctrlSock = new MockUdp;
    MockAudioOut out;
    core::airplay::AudioStream as(
        [&]() -> std::unique_ptr<core::IUdpSocket> {
            static int n = 0;
            return n++ == 0 ? std::unique_ptr<core::IUdpSocket>(dataSock)
                            : std::unique_ptr<core::IUdpSocket>(ctrlSock);
        },
        shared, scid, core::AudioCodec::AAC_LC, 48000, 2, &out);

    // 首包 seq=0 建立基线，随后乱序注入 2,1,4,3,6,5 → 期望按 0..6 顺序吐出
    // timestamp 故意 = seq+1000，用它验证吐出的顺序
    for (uint16_t seq : {uint16_t(0), uint16_t(2), uint16_t(1), uint16_t(4),
                         uint16_t(3), uint16_t(6), uint16_t(5)})
        dataSock->inject(rtp(seq, 1000 + seq));

    bool done = false;
    for (int i = 0; i < 300 && !done; ++i) {
        {
            std::lock_guard<std::mutex> lk(out.mu);
            done = out.samples.size() >= 7;
        }
        if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(done, "音频：7 个乱序包全部解密并按序吐出");

    {
        std::lock_guard<std::mutex> lk(out.mu);
        bool ordered = out.samples.size() == 7;
        for (size_t i = 0; ordered && i < out.samples.size(); ++i)
            if (out.samples[i] != 1000 + i) ordered = false;
        CHECK(ordered, "音频：吐出的 timestamp 严格按 RTP 序号升序（0..6）");
    }

    // 注入迟到的重复包 seq=2（已播放过）→ 应被丢弃，不新增输出
    dataSock->inject(rtp(2, 1002));

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    {
        std::lock_guard<std::mutex> lk(out.mu);
        CHECK(out.samples.size() == 7, "音频：迟到重复包被丢弃（不重复播放）");
    }
}

}  // namespace

int main()
{
    testTlv8();
    testChaCha();
    testHkdf();
    testX25519();
    testEd25519();
    testSrp();
    testControlCipher();
    testSessionFlow();
    testScreenStream();
    testAudioReorder();
    std::printf("\n%s（失败 %d）\n", g_fail ? "==== 有失败 ====" : "==== 全部通过 ====", g_fail);
    return g_fail ? 1 : 0;
}
