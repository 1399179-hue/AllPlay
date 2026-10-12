/**
 * @file   bplist.h
 * @brief  二进制 plist 序列化（自己实现，不引第三方）
 * @author blt
 */

// ============================================================================
// core/bplist/bplist.h —— 二进制 plist 序列化（自己实现，不引第三方）
// ============================================================================
// AirPlay 的 /info、SETUP 响应等都是 binary plist（"bplist00"）。
// 这里实现**写**这一半（M1 的 /info 需要）；**读**那一半到 M3 解析手机请求时再补，
// 放在同一组文件里保持对称。
//
// 为什么要自己写：这是协议的一部分（编解码自实现，不引第三方），
// 而且只用到一个很小的子集：dict/array/string/data/整数/布尔/浮点。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <cstddef>

namespace core::bplist {

class Value;
using Ptr = std::shared_ptr<Value>;

// ---- 构造（全部返回共享指针，树形组合）----
Ptr boolean(bool v);
Ptr u(uint64_t v);
Ptr i(int64_t v);
Ptr real(double v);
Ptr str(std::string v);
Ptr data(std::vector<uint8_t> v);
Ptr array(std::vector<Ptr> v);
/// 字典：保持插入顺序（plist 读取端不要求有序）
Ptr dict(std::vector<std::pair<std::string, Ptr>> v);

/// 序列化成 "bplist00" 格式的字节流（内部：对象扁平化 + 偏移表 + trailer）
std::vector<uint8_t> serialize(const Ptr& root);

// ---------------------------------------------------------------------------
// 读侧（M3 起：解析手机发来的 bplist 请求体，如 SETUP / /command）
// ---------------------------------------------------------------------------
// 注意（对照 bplist 编码的读侧）：8 字节整数按**无符号**读——手机的
// streamConnectionID / seed 高位置位时会被（错误地）解成负数 Long，必须当 u64。
struct Node;
using NodePtr = std::shared_ptr<Node>;

enum class NodeType { Dict, Array, Str, Data, Uint, Int, Real, Bool, Null };

struct Node {
    NodeType type = NodeType::Null;

    std::vector<std::pair<std::string, NodePtr>> dict;   ///< Dict（保序）
    std::vector<NodePtr>                              array; ///< Array / Set
    std::string        str;
    std::vector<uint8_t> data;
    uint64_t           u = 0;
    int64_t            i = 0;
    double             r = 0.0;
    bool               b = false;
};

/// 解析 "bplist00"；失败（魔数/trailer/越界）返回 nullptr。
NodePtr parse(const uint8_t* data, size_t len);

/// dict 按键取值；不是 dict 或没有该键返回 nullptr。
NodePtr dictGet(const NodePtr& n, const char* key);
/// array 取下标；越界/不是 array 返回 nullptr。
NodePtr arrGet(const NodePtr& n, size_t idx);

std::string         strOf(const NodePtr& n);   ///< Str；否则 ""
std::vector<uint8_t> dataOf(const NodePtr& n);  ///< Data；否则空
uint64_t            uintOf(const NodePtr& n);   ///< Uint/Int；否则 0
bool                boolOf(const NodePtr& n);   ///< Bool；否则 false

}  // namespace core::bplist
