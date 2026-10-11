#include "crypto/chacha20poly1305.h"

#include <openssl/evp.h>

namespace core::crypto {

std::array<uint8_t, 12> nonce64(uint64_t counter)
{
    std::array<uint8_t, 12> n{};
    for (int i = 0; i < 8; ++i) n[4 + i] = uint8_t(counter >> (8 * i));
    return n;
}

std::array<uint8_t, 12> nonceLabel(const std::string& label)
{
    std::array<uint8_t, 12> n{};
    for (size_t i = 0; i < label.size() && i < 8; ++i)
        n[4 + i] = uint8_t(label[i]);
    return n;
}

std::vector<uint8_t> chachaSeal(const uint8_t key[32], const std::array<uint8_t, 12>& nonce,
                                const uint8_t* aad, size_t aadLen,
                                const uint8_t* pt, size_t ptLen)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    std::vector<uint8_t> out(ptLen + 16);
    int len = 0, fin = 0;

    EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr);
    EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce.data());
    if (aadLen) EVP_EncryptUpdate(ctx, nullptr, &len, aad, int(aadLen));
    if (ptLen)  EVP_EncryptUpdate(ctx, out.data(), &len, pt, int(ptLen));
    EVP_EncryptFinal_ex(ctx, out.data() + len, &fin);
    len += fin;

    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, out.data() + len);
    EVP_CIPHER_CTX_free(ctx);
    out.resize(len + 16);
    return out;
}

bool chachaOpen(const uint8_t key[32], const std::array<uint8_t, 12>& nonce,
                const uint8_t* aad, size_t aadLen,
                const uint8_t* ct, size_t ctLen,
                std::vector<uint8_t>* out)
{
    if (ctLen < 16) return false;
    const size_t ptLen = ctLen - 16;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    out->assign(ptLen, 0);
    int len = 0, fin = 0;

    EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr);
    EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce.data());
    if (aadLen) EVP_DecryptUpdate(ctx, nullptr, &len, aad, int(aadLen));
    if (ptLen)  EVP_DecryptUpdate(ctx, out->data(), &len, ct, int(ptLen));
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, const_cast<uint8_t*>(ct + ptLen));

    int r = EVP_DecryptFinal_ex(ctx, out->data() + len, &fin);
    EVP_CIPHER_CTX_free(ctx);

    if (r <= 0) { out->clear(); return false; }
    out->resize(len + fin);
    return true;
}

}  // namespace core::crypto
