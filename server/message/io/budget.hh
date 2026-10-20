#pragma once
#include "IoQueue.h"
#include <seastar/core/semaphore.hh>

namespace wimi::message_io
{
struct capacity_units {
    seastar::semaphore_units<> count;
    seastar::semaphore_units<> bytes;
};
// 每个 shard 的全部流共享容量；帧/请求持有额度直到完成，额度不足立即拒绝。
class budget
{
    seastar::semaphore _count;
    seastar::semaphore _bytes;

  public:
    budget(size_t count, size_t bytes) : _count(count), _bytes(bytes) {}
    capacity_units acquire(size_t bytes)
    {
        auto count = seastar::try_get_units(_count, 1);
        auto storage = seastar::try_get_units(_bytes, bytes);
        if (!count || !storage) {
            throw io::overloaded();
        }
        return {std::move(*count), std::move(*storage)};
    }
};
} // namespace wimi::message_io
