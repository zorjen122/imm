#pragma once
#include "IoQueue.h"
#include <seastar/core/semaphore.hh>

namespace wimi::gateway_io
{
struct capacity_units {
    seastar::semaphore_units<> count;
    seastar::semaphore_units<> bytes;
};
// 所有 Message 流共享所属 shard 的额度。持有者跨挂起点保留 units，
// 排队失败、写完成、请求交付或取消时由 RAII 归还，不创建等待容量的任务。
class budget
{
    seastar::semaphore _count;
    seastar::semaphore _bytes;

  public:
    budget(size_t count, size_t bytes) : _count(count), _bytes(bytes) {}
    void resize(capacity_units& units, size_t bytes)
    {
        const auto old = units.bytes.count();
        if (bytes > old) {
            auto extra = seastar::try_get_units(_bytes, bytes - old);
            if (!extra) {
                throw io::overloaded();
            }
            units.bytes.adopt(std::move(*extra));
        } else {
            units.bytes.return_units(old - bytes);
        }
    }
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
} // namespace wimi::gateway_io
