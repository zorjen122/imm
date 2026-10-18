#include "io/redis_connection.hh"
#include "io/fd_watch.hh"
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/when_all.hh>
#include <cassert>
#include <fcntl.h>
#include <functional>
#include <stdexcept>
#include <system_error>

namespace wimi::gate_io
{

struct command_completion {
    seastar::promise<redis_reply> result;
};

redis_connection::redis_connection()
    : _timer([this] {
          if (_context) {
              redisAsyncHandleTimeout(_context);
          }
      }),
      _owner(seastar::this_shard_id())
{
}
redis_connection::~redis_connection() { assert(!_context && !_stopped); }
void redis_connection::connected(const redisAsyncContext* context, int status)
{
    auto& self = *static_cast<redis_connection*>(context->data);
    assert(self._owner == seastar::this_shard_id());
    if (!self._connecting) {
        return;
    }
    self._connecting = false;
    if (status == REDIS_OK) {
        self._connected.set_value();
    } else {
        self._connected.set_exception(std::runtime_error(context->errstr));
    }
}
void redis_connection::cleanup(void* state)
{
    auto& self = *static_cast<redis_connection*>(state);
    self._context = nullptr;
    self._timer.cancel();
    self._stopped.emplace(self._watch->stop());
}
seastar::future<> redis_connection::connect(std::string numeric_ip, uint16_t port)
{
    assert(_owner == seastar::this_shard_id());
    timeval timeout{1, 0};
    redisOptions options{};
    REDIS_OPTIONS_SET_TCP(&options, numeric_ip.c_str(), port);
    options.connect_timeout = &timeout;
    options.command_timeout = &timeout;
    _context = redisAsyncConnectWithOptions(&options);
    if (!_context || _context->err) {
        const std::string error = _context ? _context->errstr : "Redis allocation failed";
        if (_context) {
            redisAsyncFree(std::exchange(_context, nullptr));
        }
        throw std::runtime_error(error);
    }
    _watch = fd_watch::create(
        _context->c.fd,
        [this] {
            if (_context) {
                redisAsyncHandleRead(_context);
            }
        },
        [this] {
            if (_context) {
                redisAsyncHandleWrite(_context);
            }
        });
    // hiredis 通过这些钩子声明读写兴趣和计时需求；真正的等待由 reactor 完成，
    // 就绪后才回调 redisAsyncHandleRead/Write，不启动 hiredis 的独立事件循环。
    _context->data = this;
    _context->ev.data = this;
    _context->ev.addRead = [](void* state) { static_cast<redis_connection*>(state)->_watch->set(0, true); };
    _context->ev.delRead = [](void* state) { static_cast<redis_connection*>(state)->_watch->set(0, false); };
    _context->ev.addWrite = [](void* state) { static_cast<redis_connection*>(state)->_watch->set(1, true); };
    _context->ev.delWrite = [](void* state) { static_cast<redis_connection*>(state)->_watch->set(1, false); };
    _context->ev.cleanup = cleanup;
    _context->ev.scheduleTimer = [](void* state, timeval duration) {
        auto& self = *static_cast<redis_connection*>(state);
        self._timer.cancel();
        self._timer.arm(std::chrono::steady_clock::now() + std::chrono::seconds(duration.tv_sec) +
                        std::chrono::microseconds(duration.tv_usec));
    };
    _connecting = true;
    auto ready = _connected.get_future();
    redisAsyncSetConnectCallback(_context, connected);
    co_await std::move(ready);
}
void redis_connection::completed(redisAsyncContext*, void* data, void* state)
{
    std::unique_ptr<command_completion> completion(static_cast<command_completion*>(state));
    auto* reply = static_cast<redisReply*>(data);
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        completion->result.set_exception(
            std::runtime_error(reply ? std::string(reply->str, reply->len) : "Redis connection closed"));
        return;
    }
    // redisReply 属于 hiredis，回调返回后不可继续借用；先复制为自有数据再完成 promise。
    redis_reply result;
    result.type = reply->type;
    result.integer = reply->integer;
    if (reply->str) {
        result.text.assign(reply->str, reply->len);
    }
    if (reply->type == REDIS_REPLY_ARRAY) {
        for (size_t index = 0; index < reply->elements; ++index) {
            auto* item = reply->element[index];
            if (!item->str) {
                completion->result.set_exception(std::runtime_error("invalid Sentinel response"));
                return;
            }
            result.elements.emplace_back(item->str, item->len);
        }
    }
    completion->result.set_value(std::move(result));
}
seastar::future<redis_reply> redis_connection::command(std::vector<std::string> arguments)
{
    assert(_owner == seastar::this_shard_id());
    auto completion = std::make_unique<command_completion>();
    std::vector<const char*> values;
    std::vector<size_t> sizes;
    for (const std::string& value : arguments) {
        values.push_back(value.data());
        sizes.push_back(value.size());
    }
    if (!_context || redisAsyncCommandArgv(_context, completed, completion.get(), values.size(), values.data(),
                                           sizes.data()) != REDIS_OK) {
        return seastar::make_exception_future<redis_reply>(std::runtime_error("Redis command submission failed"));
    }
    auto result = completion->result.get_future();
    // 提交成功后由 completed() 接管并释放状态；断连时 hiredis 也会以空 reply
    // 调用待完成命令的回调，统一向等待者交付失败。
    completion.release();
    return result;
}
void redis_connection::abort()
{
    assert(_owner == seastar::this_shard_id());
    if (_connecting) {
        _connecting = false;
        _connected.set_exception(std::runtime_error("Redis connect aborted"));
    }
    if (_context) {
        redisAsyncFree(std::exchange(_context, nullptr));
    }
}
seastar::future<> redis_connection::close()
{
    abort();
    if (_stopped) {
        co_await std::move(*_stopped);
        _stopped.reset();
    }
    _watch.reset();
}
} // namespace wimi::gate_io
