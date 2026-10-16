// ============================================================================
// tools/wired_selftest.cpp —— 有线链路里"不需要手机"的那部分，离线自检
// ============================================================================
// 有线这条路的失败模式很讨厌：真机上一卡就是超时，而超时既可能是"手机不理我"，
// 也可能是"我自己的代码写错了"。所以凡是**不用插手机就能验**的，都在这里验掉：
//
//   ① XML plist 解析 —— 手机回话带 XML 声明与 DOCTYPE，解析器必须吃得下
//   ② 证书结构     —— serial 0 / 空 DN / 扩展与 critical 位 / device 那张的公钥
//   ③ TLS 回环握手 —— 自己跟自己握一次，验 BIO 管道、读写、以及 **root 证书能不能
//                     当客户端身份用**（这一步真机上失败了极难定位）
//
// 这些都是纯软件，在本机/9.202 上就能跑：wired_probe --selftest
#include "usb/pair_record.h"
#include "usb/plist_xml.h"
#include "usb/tls_stream.h"

#include <openssl/bio.h>
#include <openssl/encoder.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_bad = 0;

void check(bool ok, const std::string& what)
{
    std::printf("  %s %s\n", ok ? "✓" : "✗", what.c_str());
    if (!ok) ++g_bad;
}

std::string opensslError()
{
    std::string out;
    unsigned long e = 0;
    while ((e = ERR_get_error()) != 0) {
        char buf[256] = "";
        ERR_error_string_n(e, buf, sizeof buf);
        if (!out.empty()) out += " / ";
        out += buf;
    }
    return out.empty() ? "（无错误码）" : out;
}

// ---------------------------------------------------------------- ① plist ----

void testPlist()
{
    std::printf("=== ① XML plist 解析 ===\n");

    // 手机回的 plist 实测就长这样：带 XML 声明 + DOCTYPE
    const std::string phoneReply =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>"
        "<key>Type</key><string>com.apple.mobile.lockdown</string>"
        "<key>Port</key><integer>49152</integer>"
        "<key>EnableSessionSSL</key><true/>"
        "<key>Off</key><false/>"
        "<key>Blob</key><data>AAECAwQ=</data>"
        "</dict></plist>";

    core::usb::Plist p;
    std::string     err;
    check(core::usb::Plist::parse(phoneReply, &p, &err), "解析带声明的回话（" + err + "）");
    if (!p.isDict()) {
        check(false, "顶层是 dict");
        return;
    }
    check(p.stringValue("Type") == "com.apple.mobile.lockdown", "string 字段");
    check(p.intValue("Port") == 49152, "integer 字段");
    check(p.boolValue("EnableSessionSSL", false), "true 字段");
    check(!p.boolValue("Off", true), "false 字段");

    const core::usb::Plist* blob = p.find("Blob");
    const bool blobOk = blob != nullptr && blob->type() == core::usb::Plist::Type::Data &&
                        blob->asData() == std::vector<uint8_t>({0, 1, 2, 3, 4});
    check(blobOk, "data 字段（base64 解码）");

    // 自产自销：自己生成的请求也要能被自己解回来（锁的是成帧，不是内容）
    core::usb::Plist req = core::usb::Plist::dict();
    req.set("Request", core::usb::Plist::string("Pair"));
    req.set("ProtocolVersion", core::usb::Plist::string("2"));
    core::usb::Plist back;
    check(core::usb::Plist::parse(req.toXml(), &back, &err), "自己发的 XML 能解回来");
    check(back.stringValue("Request") == "Pair" && back.stringValue("ProtocolVersion") == "2",
          "往返值一致");

    // 空 DN 的证书要放进 <data>，先确认 base64 编解码在长数据上没问题
    std::vector<uint8_t> big(1000);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 7 + 3);
    core::usb::Plist d = core::usb::Plist::dict();
    d.set("D", core::usb::Plist::data(big));
    core::usb::Plist d2;
    check(core::usb::Plist::parse(d.toXml(), &d2, &err), "1KB <data> 往返");
    const core::usb::Plist* dv = d2.find("D");
    check(dv != nullptr && dv->asData() == big, "1KB <data> 内容一致");
}

// ------------------------------------------------------------- ② 证书 ----

X509* pemToX509(const std::string& pem)
{
    BIO*  b = BIO_new_mem_buf(pem.data(), int(pem.size()));
    X509* x = b != nullptr ? PEM_read_bio_X509(b, nullptr, nullptr, nullptr) : nullptr;
    if (b != nullptr) BIO_free(b);
    return x;
}

/// 取扩展的 critical 位。返回 false = 没有这条扩展。
bool extCritical(X509* x, int nid, bool* critical)
{
    int crit = -1;
    void* v = X509_get_ext_d2i(x, nid, &crit, nullptr);
    if (v == nullptr) return false;
    OPENSSL_free(v);
    *critical = (crit == 1);
    return true;
}

/// 把公钥编成 PKCS#1 RSAPublicKey DER（跟手机给的那个格式对齐）
std::vector<uint8_t> pkcs1PublicDer(EVP_PKEY* key)
{
    std::vector<uint8_t> der;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    OSSL_ENCODER_CTX* ectx = OSSL_ENCODER_CTX_new_for_pkey(key, EVP_PKEY_PUBLIC_KEY, "DER",
                                                           "type-specific", nullptr);
    if (ectx == nullptr) return der;
    unsigned char* out = nullptr;
    size_t         n = 0;
    if (OSSL_ENCODER_to_data(ectx, &out, &n) == 1 && out != nullptr) {
        der.assign(out, out + n);
        OPENSSL_free(out);
    }
    OSSL_ENCODER_CTX_free(ectx);
#endif
    return der;
}

void testCerts()
{
    std::printf("\n=== ② 配对证书结构 ===\n");

    // 假造一把"手机公钥"：就是一把普通 RSA 公钥的 PKCS#1 编码
    EVP_PKEY* phoneKey = nullptr;
    {
        EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY_keygen_init(c);
        EVP_PKEY_CTX_set_rsa_keygen_bits(c, 2048);
        EVP_PKEY_keygen(c, &phoneKey);
        EVP_PKEY_CTX_free(c);
    }
    const std::vector<uint8_t> phonePkcs1 = pkcs1PublicDer(phoneKey);
    check(!phonePkcs1.empty(), "造出一把 2048 位测试公钥");

    // 真机实测：GetValue(DevicePublicKey) 回来的是 **PEM 文本**，不是文档说的 PKCS#1 DER。
    // 两种形态都必须能吃下 —— 这条回归踩过一次，别再靠真机才发现。
    std::string phonePem;
    {
        BIO* b = BIO_new(BIO_s_mem());
        PEM_write_bio_PUBKEY(b, phoneKey);
        char*      p = nullptr;
        const long n = BIO_get_mem_data(b, &p);
        if (p != nullptr && n > 0) phonePem.assign(p, size_t(n));
        BIO_free(b);
    }
    EVP_PKEY_free(phoneKey);
    check(!phonePem.empty(), "同一把公钥也能编成 PEM 文本");

    core::usb::WiredPairIdentity id;
    std::string                 err;
    const int                   before = g_bad;
    check(core::usb::makeWiredPairIdentity(phonePkcs1, "", "", &id, &err),
          "生成 root/host/device 三张证书（" + err + "）");
    if (g_bad != before) return;  // 证书都没造出来，后面的检查没意义

    {
        core::usb::WiredPairIdentity id2;
        std::string                  err2;
        const std::vector<uint8_t>   pemBytes(phonePem.begin(), phonePem.end());
        check(core::usb::makeWiredPairIdentity(pemBytes, "", "", &id2, &err2),
              "PEM 形态的手机公钥也能用（" + err2 + "）");
        X509*     d2 = pemToX509(id2.deviceCertPem);
        EVP_PKEY* dp = d2 != nullptr ? X509_get0_pubkey(d2) : nullptr;
        check(dp != nullptr && pkcs1PublicDer(dp) == phonePkcs1,
              "PEM 与 PKCS#1 两条路得到的是同一把公钥");
        if (d2 != nullptr) X509_free(d2);
    }

    check(id.rootCertPem.rfind("-----BEGIN CERTIFICATE-----", 0) == 0, "证书是 CERTIFICATE PEM");
    check(id.rootKeyPem.rfind("-----BEGIN PRIVATE KEY-----", 0) == 0, "私钥是 PKCS#8 PEM");

    std::string upper = id.hostId;
    for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    check(id.hostId.size() == 36 && id.hostId == upper && id.hostId[8] == '-' &&
              id.hostId[13] == '-' && id.hostId[18] == '-' && id.hostId[23] == '-',
          "HostID 是 36 位大写 UUID");
    check(id.systemBuid != id.hostId && id.systemBuid.size() == 36, "SystemBUID 另给了一个");

    X509* root = pemToX509(id.rootCertPem);
    X509* host = pemToX509(id.hostCertPem);
    X509* dev = pemToX509(id.deviceCertPem);
    check(root && host && dev, "三张证书都能重新读出来");
    if (!root || !host || !dev) return;

    check(ASN1_INTEGER_get(X509_get_serialNumber(root)) == 0, "serial 是 0");
    check(X509_NAME_entry_count(X509_get_subject_name(root)) == 0, "subject DN 是空的");
    check(X509_NAME_entry_count(X509_get_issuer_name(root)) == 0, "issuer DN 是空的");
    check(X509_get_version(root) == 2, "是 v3 证书");

    // 有效期跨度：正好 10 年
    int days = 0, secs = 0;
    ASN1_TIME_diff(&days, &secs, X509_get0_notBefore(root), X509_get0_notAfter(root));
    check(days == 3650, "有效期正好 3650 天（实测 " + std::to_string(days) + "）");

    bool crit = false;
    check(extCritical(root, NID_basic_constraints, &crit) && crit, "root 的 basicConstraints 是 critical");
    check(X509_check_ca(root) == 1, "root 是 CA");

    check(extCritical(host, NID_basic_constraints, &crit) && crit,
          "host 的 basicConstraints 是 critical");
    check(X509_check_ca(host) == 0, "host 不是 CA");
    const uint32_t ku = X509_get_key_usage(host);
    check((ku & (KU_DIGITAL_SIGNATURE | KU_KEY_ENCIPHERMENT)) ==
              (KU_DIGITAL_SIGNATURE | KU_KEY_ENCIPHERMENT),
          "host 的 keyUsage 含 digitalSignature + keyEncipherment");

    check(!extCritical(host, NID_subject_key_identifier, &crit), "host **没有** SKI");
    check(extCritical(dev, NID_subject_key_identifier, &crit) && !crit, "device 有 SKI 且非 critical");
    check(X509_check_ca(dev) == 0, "device 不是 CA");

    // device 那张的公钥必须就是手机给的那把
    EVP_PKEY* devPub = X509_get0_pubkey(dev);
    check(devPub != nullptr && pkcs1PublicDer(devPub) == phonePkcs1,
          "device 证书里的公钥 = 手机给的公钥");

    X509_free(root);
    X509_free(host);
    X509_free(dev);
}

// ------------------------------------------------------------- ③ TLS ----

/// 把一条 fd 包成 IByteStream，好让 startTlsClient 有东西可套
class FdStream final : public core::IByteStream {
public:
    explicit FdStream(int fd) : fd_(fd) {}
    ~FdStream() override { close(); }

    int read(uint8_t* buf, size_t n, int timeoutMs) override
    {
        if (fd_ < 0) return -1;
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, timeoutMs);
        if (r == 0) return 0;   // 超时
        if (r < 0) return -1;
        const ssize_t k = ::recv(fd_, buf, n, 0);
        return k < 0 ? -1 : int(k);
    }
    bool writeAll(const uint8_t* buf, size_t n) override
    {
        if (fd_ < 0) return false;
        size_t off = 0;
        while (off < n) {
            const ssize_t k = ::send(fd_, buf + off, n - off, MSG_NOSIGNAL);
            if (k <= 0) return false;
            off += size_t(k);
        }
        return true;
    }
    void close() override
    {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

private:
    int fd_;
};

/// 假手机：用**同一套证书**接一次 TLS，收一句回一句
void fakePhone(int fd, const std::string& certPem, const std::string& keyPem, bool* ok)
{
    *ok = false;
    // 客户端要是没来（比如那边证书就装不上），别让这条线程把 join 卡死
    timeval tv{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr) return;
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    BIO* cb = BIO_new_mem_buf(certPem.data(), int(certPem.size()));
    X509* x = PEM_read_bio_X509(cb, nullptr, nullptr, nullptr);
    BIO_free(cb);
    BIO* kb = BIO_new_mem_buf(keyPem.data(), int(keyPem.size()));
    EVP_PKEY* k = PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr);
    BIO_free(kb);
    if (x == nullptr || k == nullptr || SSL_CTX_use_certificate(ctx, x) != 1 ||
        SSL_CTX_use_PrivateKey(ctx, k) != 1) {
        std::printf("    （假手机装证书失败：%s）\n", opensslError().c_str());
        goto done;
    }

    {
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        if (SSL_accept(ssl) == 1) {
            char buf[64] = "";
            const int n = SSL_read(ssl, buf, sizeof buf);
            if (n > 0) {
                if (SSL_write(ssl, "pong", 4) == 4) *ok = true;
                std::printf("    （假手机收到 %d 字节：%.*s）\n", n, n, buf);
            }
        } else {
            std::printf("    （假手机握手失败：%s）\n", opensslError().c_str());
        }
        SSL_free(ssl);
    }

done:
    if (x != nullptr) X509_free(x);
    if (k != nullptr) EVP_PKEY_free(k);
    SSL_CTX_free(ctx);
}

void testTls()
{
    std::printf("\n=== ③ TLS 回环握手 ===\n");

    core::usb::WiredPairIdentity id;
    std::string                 err;
    EVP_PKEY* pk = nullptr;
    {
        EVP_PKEY_CTX* c = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        EVP_PKEY_keygen_init(c);
        EVP_PKEY_CTX_set_rsa_keygen_bits(c, 2048);
        EVP_PKEY_keygen(c, &pk);
        EVP_PKEY_CTX_free(c);
    }
    const std::vector<uint8_t> phonePkcs1 = pkcs1PublicDer(pk);
    EVP_PKEY_free(pk);
    if (!core::usb::makeWiredPairIdentity(phonePkcs1, "", "", &id, &err)) {
        check(false, "造证书（" + err + "）");
        return;
    }

    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        check(false, "socketpair 失败");
        return;
    }

    bool serverOk = false;
    // 客户端身份用 **root** —— 跟真机上保持一致，这点搞反了真机才会暴露
    std::thread server(fakePhone, fds[1], id.rootCertPem, id.rootKeyPem, &serverOk);

    FdStream raw(fds[0]);
    auto     tls = core::usb::startTlsClient(raw, id.rootCertPem, id.rootKeyPem, &err);
    check(tls != nullptr, "客户端握手完成（" + err + "）");

    if (tls != nullptr) {
        const char* ping = "ping";
        check(tls->writeAll(reinterpret_cast<const uint8_t*>(ping), 4), "经 TLS 写出去");

        char   buf[64] = "";
        size_t got = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (got < 4 && std::chrono::steady_clock::now() < deadline) {
            const int n = tls->read(reinterpret_cast<uint8_t*>(buf) + got, sizeof(buf) - got, 500);
            if (n < 0) break;
            if (n > 0) got += size_t(n);
        }
        check(got == 4 && std::memcmp(buf, "pong", 4) == 0, "经 TLS 读回来");
    }

    server.join();  // 假手机要么握完手退出，要么等客户端 close 后超时退出
    check(serverOk, "服务端那一侧也认为握手成功");
}

}  // namespace

/// wired_probe --selftest 的入口
int wiredSelfTest()
{
    std::printf("=== 有线链路离线自检（不需要手机）===\n\n");
    testPlist();
    testCerts();
    testTls();
    std::printf("\n%s（%d 项不通过）\n", g_bad == 0 ? "全部通过" : "有失败", g_bad);
    return g_bad == 0 ? 0 : 1;
}
