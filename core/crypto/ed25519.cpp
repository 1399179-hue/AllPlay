#include "crypto/ed25519.h"

namespace core::crypto {

std::vector<uint8_t> ed25519Sign(EVP_PKEY* key, const uint8_t* msg, size_t len)
{
    if (!key) return {};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    std::vector<uint8_t> sig;
    if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1) {
        size_t siglen = 0;
        if (EVP_DigestSign(ctx, nullptr, &siglen, msg, len) == 1 && siglen > 0) {
            sig.resize(siglen);
            if (EVP_DigestSign(ctx, sig.data(), &siglen, msg, len) != 1) sig.clear();
        }
    }
    EVP_MD_CTX_free(ctx);
    return sig;
}

bool ed25519Verify(const uint8_t pub[32], const uint8_t* msg, size_t len, const uint8_t sig[64])
{
    EVP_PKEY* pk = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub, 32);
    if (!pk) return false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    bool ok = false;
    if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pk) == 1)
        ok = EVP_DigestVerify(ctx, sig, 64, msg, len) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return ok;
}

}  // namespace core::crypto
