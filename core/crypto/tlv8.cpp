#include "crypto/tlv8.h"

#include <algorithm>

namespace core::crypto {

std::vector<uint8_t> tlv8Encode(const std::vector<Tlv>& items)
{
    std::vector<uint8_t> out;
    bool first = true;
    uint8_t lastType = 0xFF;

    for (const auto& it : items) {
        // 相邻两条条目 type 相同 → 插分隔 (0xff, 0x00)
        if (!first && it.type == lastType) {
            out.push_back(0xFF);
            out.push_back(0x00);
        }
        size_t off = 0;
        do {
            const size_t n = std::min<size_t>(255, it.value.size() - off);
            out.push_back(it.type);
            out.push_back(uint8_t(n));
            out.insert(out.end(), it.value.begin() + off, it.value.begin() + off + n);
            off += n;
        } while (off < it.value.size());
        lastType = it.type;
        first = false;
    }
    return out;
}

std::vector<Tlv> tlv8Decode(const uint8_t* data, size_t len)
{
    std::vector<Tlv> out;
    uint8_t lastType = 0xFF;
    uint8_t lastLen  = 0;
    size_t i = 0;

    while (i + 2 <= len) {
        const uint8_t type = data[i];
        const uint8_t n    = data[i + 1];
        i += 2;
        if (i + n > len) break;   // 截断
        std::vector<uint8_t> val(data + i, data + i + n);
        i += n;

        // 续段：同 type 且上一片 len==255 → 追加
        if (type == lastType && lastLen == 255) {
            for (auto& t : out) {
                if (t.type == type) {
                    t.value.insert(t.value.end(), val.begin(), val.end());
                    break;
                }
            }
        } else {
            // 覆盖同 type（只保留最后一条）
            out.erase(std::remove_if(out.begin(), out.end(),
                                     [&](const Tlv& t) { return t.type == type; }),
                      out.end());
            out.push_back({type, std::move(val)});
        }
        lastType = type;
        lastLen  = n;
    }
    return out;
}

const std::vector<uint8_t>* tlv8Get(const std::vector<Tlv>& items, uint8_t type)
{
    for (const auto& t : items)
        if (t.type == type) return &t.value;
    return nullptr;
}

}  // namespace core::crypto
