/**
 * @file   tlv8.h
 * @brief  TLV8 编解码（pair-setup / pair-verify 的消息体）
 * @author blt
 */

// ============================================================================
// core/crypto/tlv8.h —— TLV8 编解码（pair-setup / pair-verify 的消息体）
// ============================================================================
// 每条 = [type:1][len:1][value:len]。value>255 按 255 分片（每片带 type/len 头）。
// 相邻两条**条目** type 相同时，中间插分隔符 (0xff, 0x00)。解码时 0xff 无特殊语义，
// 续段靠"同 type 且上一片 len==255"判断，否则覆盖。
#pragma once

#include <cstdint>
#include <vector>

namespace core::crypto {

struct Tlv {
    uint8_t             type;
    std::vector<uint8_t> value;
};

/// 编码（按 items 顺序；相邻同 type 插分隔；>255 分片）。
std::vector<uint8_t> tlv8Encode(const std::vector<Tlv>& items);

/// 解码（分片拼接 + 同 type 覆盖语义）。截断不报错，解出多少算多少。
std::vector<Tlv> tlv8Decode(const uint8_t* data, size_t len);

/// 从解码结果取某 type 的 value；不存在返回 nullptr。
const std::vector<uint8_t>* tlv8Get(const std::vector<Tlv>& items, uint8_t type);

}  // namespace core::crypto
