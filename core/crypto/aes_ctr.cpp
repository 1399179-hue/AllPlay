#include "crypto/aes_ctr.h"

#include <openssl/evp.h>

namespace core::crypto {

std::vector<uint8_t> aes128Ctr(const uint8_t key[16], const uint8_t iv[16],
                               const uint8_t* data, size_t len)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    std::vector<uint8_t> out(len);
    int olen = 0, fin = 0;

    EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key, iv);
    if (len) EVP_EncryptUpdate(ctx, out.data(), &olen, data, int(len));
    EVP_EncryptFinal_ex(ctx, out.data() + olen, &fin);
    EVP_CIPHER_CTX_free(ctx);
    out.resize(olen + fin);
    return out;
}

}  // namespace core::crypto
