#include "business/service.hh"
#include "Logger.h"
#include <seastar/core/coroutine.hh>
#include <sys/syscall.h>
#include <unistd.h>

namespace wimi::gate_business
{
using namespace gate_io;
service::service(options config, seastar::scheduling_group transaction_group, seastar::scheduling_group io_group)
    : _options(std::move(config)), _transaction_group(transaction_group), _driver(_options),
      _io(_driver, _options.queue, io_group), _request_count(_options.max_requests),
      _request_bytes(_options.max_request_bytes)
{
}

seastar::future<> service::start()
{
    co_await _driver.start();
    _io.start();
    LOG_INFO(logger, "[gate/io] ready shard={} tid={}", seastar::this_shard_id(), syscall(SYS_gettid));
}
request_permit service::admit(size_t bytes)
{
    if (_stopping) {
        throw io::stopped();
    }
    // 额度是每 shard 独立的，字节数按原始 payload 计，不代表解码后对象的精确内存。
    // 两次获取都不挂起；任一失败时，已获取的另一份额度也由局部 RAII 对象归还。
    auto count = seastar::try_get_units(_request_count, 1);
    auto storage = seastar::try_get_units(_request_bytes, bytes);
    if (!count || !storage) {
        throw io::overloaded();
    }
    return {_requests.hold(), std::move(*count), std::move(*storage)};
}
seastar::future<> service::stop()
{
    if (_stopped) {
        co_return;
    }
    // 先拒绝业务准入与待执行 I/O，再中止在途传输，让等待中的消费者能够退出。
    // 随后等待请求完成异常处理和响应编码，最后才能释放驱动及 shard 级依赖。
    _stopping = true;
    _io.begin_stop();
    _driver.abort();
    co_await _io.stop();
    co_await _requests.close();
    co_await _driver.stop();
    _stopped = true;
}
} // namespace wimi::gate_business
