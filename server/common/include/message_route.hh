#pragma once
#include <cassert>
#include <cstdint>
#include <string_view>

namespace imm::message_route
{
// Gateway 与 Message 共用确定性散列；不能用实现相关的 std::hash 约定端口。
// 调用前须验证 shard_count > 0，端口范围及实际 Message shard 数另行校验。
inline unsigned owner_shard(std::string_view gateway_id, unsigned shard_count)
{
    assert(shard_count > 0);
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : gateway_id) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash % shard_count;
}
} // namespace imm::message_route
