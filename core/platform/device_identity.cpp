#include "platform/device_identity.h"

#include "common/log.h"

#include <openssl/pem.h>
#include <openssl/rand.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace core {

namespace fs = std::filesystem;

namespace {

std::string hexOf(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(d[p[i] >> 4]);
        out.push_back(d[p[i] & 0xF]);
    }
    return out;
}

std::string makeUuid()
{
    uint8_t b[16];
    if (RAND_bytes(b, sizeof b) != 1) {
        // 极端情况（RAND 失败）：用时间凑一个，别让程序起不来
        for (size_t i = 0; i < sizeof b; ++i) b[i] = uint8_t(rand());
    }
    b[6] = uint8_t((b[6] & 0x0F) | 0x40);   // version 4
    b[8] = uint8_t((b[8] & 0x3F) | 0x80);   // variant
    char s[37];
    std::snprintf(s, sizeof s,
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                  b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

bool writeFile0600(const fs::path& p, const std::string& data)
{
    // .string().c_str()：Windows 上 path::value_type 是 wchar_t，fopen 要 char*
    FILE* f = std::fopen(p.string().c_str(), "w");
    if (!f) return false;
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

}  // namespace

std::string loadOrCreateSyntheticDeviceId(const std::string& stateDir, bool* created)
{
    if (created) *created = false;
    const fs::path p = fs::path(stateDir) / "device_id";
    if (fs::exists(p)) {
        std::ifstream in(p);
        std::string id;
        std::getline(in, id);
        if (!id.empty()) return id;
    }
    // 生成："02:xx:xx:xx:xx:xx"（本地管理位=1、单播；"02:00:…" 那个占位值避开）
    uint8_t b[6] = {0};
    if (RAND_bytes(b, sizeof b) != 1) {
        for (size_t i = 0; i < sizeof b; ++i) b[i] = uint8_t(rand());
    }
    b[0] = 0x02;
    if (b[1] == 0 && b[2] == 0 && b[3] == 0 && b[4] == 0 && b[5] == 0) b[5] = 1;
    char buf[18];
    std::snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X",
                  b[0], b[1], b[2], b[3], b[4], b[5]);
    const std::string id = buf;
    writeFile0600(p, id + "\n");
    if (created) *created = true;
    return id;
}

DeviceIdentity::~DeviceIdentity()
{
    if (priv) EVP_PKEY_free(priv);
}

std::string defaultStateDir()
{
    if (const char* e = std::getenv("ALLPLAY_STATE_DIR"); e && *e) return e;
    if (const char* h = std::getenv("HOME"); h && *h)
        return std::string(h) + "/.local/state/allplay";
    return "/tmp/allplay";
}

bool DeviceIdentity::loadOrCreate(const std::string& stateDir, std::string* err)
{
    std::error_code ec;
    fs::create_directories(stateDir, ec);
    if (ec) {
        if (err) *err = "建 state 目录失败：" + stateDir + "（" + ec.message() + "）";
        return false;
    }
    const fs::path keyPath = fs::path(stateDir) / "ed25519.pem";
    const fs::path piPath  = fs::path(stateDir) / "pairing_id";

    // ---- Ed25519 私钥：有读、没生成 ----
    if (fs::exists(keyPath)) {
        FILE* f = std::fopen(keyPath.string().c_str(), "r");
        if (!f) {
            if (err) *err = "打不开 " + keyPath.string();
            return false;
        }
        priv = PEM_read_PrivateKey(f, nullptr, nullptr, nullptr);
        std::fclose(f);
        if (!priv) {
            if (err) *err = "读私钥失败（文件坏了？删掉让它重新生成）：" + keyPath.string();
            return false;
        }
    } else {
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_keygen(ctx, &priv) <= 0) {
            if (ctx) EVP_PKEY_CTX_free(ctx);
            if (err) *err = "生成 Ed25519 密钥失败";
            return false;
        }
        EVP_PKEY_CTX_free(ctx);

        FILE* f = std::fopen(keyPath.string().c_str(), "w");
        if (!f || PEM_write_PrivateKey(f, priv, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            if (f) std::fclose(f);
            if (err) *err = "写私钥失败：" + keyPath.string();
            return false;
        }
        std::fclose(f);
        std::error_code ec;
        fs::permissions(keyPath, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
        LOG_INFO("已生成本机身份密钥：%s", keyPath.string().c_str());
    }

    // ---- 公钥 ----
    pub.resize(32);
    size_t pubLen = pub.size();
    if (EVP_PKEY_get_raw_public_key(priv, pub.data(), &pubLen) != 1) {
        if (err) *err = "取 Ed25519 公钥失败";
        return false;
    }
    pub.resize(pubLen);
    pkHex = hexOf(pub.data(), pub.size());

    // ---- 配对 UUID ----
    if (fs::exists(piPath)) {
        std::ifstream in(piPath);
        std::getline(in, pi);
    }
    if (pi.empty()) {
        pi = makeUuid();
        if (!writeFile0600(piPath, pi + "\n")) {
            if (err) *err = "写 pairing_id 失败：" + piPath.string();
            return false;
        }
        LOG_INFO("已生成配对 UUID：%s", pi.c_str());
    }
    return true;
}

}  // namespace core
