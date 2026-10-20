#pragma once

#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <concepts>
#include <type_traits>
#include <utility>
#include <cassert>
#include <seastar/core/queue.hh>
#include <seastar/core/semaphore.hh>
#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>

namespace imm::io
{

class overloaded : public std::runtime_error
{
  public:
    overloaded() : std::runtime_error("I/O queue full") {}
};
class stopped : public std::runtime_error
{
  public:
    stopped() : std::runtime_error("I/O service stopping") {}
};

struct queue_limits {
    size_t pending{128};
    size_t bytes{1024 * 1024};
    unsigned concurrency{4};
};

// 本 shard 的待执行参数和完成 promise 由队列持有；Backend::execute 的对应重载
// 返回 future<Operation::result_type>，驱动不得阻塞 reactor。容量不足立即拒绝，
// 不另建等待入队的调用者列表。销毁前，所有者须关闭准入、中止后端 I/O 并等待 stop()。
template <typename Backend, typename OperationVariant> class operation_queue;

template <typename Backend, typename... Operations> class operation_queue<Backend, std::variant<Operations...>>
{
    template <typename Operation> struct job {
        Operation operation;
        seastar::promise<typename Operation::result_type> completion;
    };
    using entry = std::variant<job<Operations>...>;
    Backend& _backend;
    queue_limits _limits;
    seastar::scheduling_group _group;
    unsigned _owner{seastar::this_shard_id()};
    bool _accepting{};
    struct pending_job {
        seastar::semaphore_units<> bytes;
        std::unique_ptr<entry> item;
    };
    seastar::semaphore _bytes;
    // 这里只使用队列的立即 push/pop；同一 reactor 上没有跨线程争用。
    // 多消费者通过 condition_variable 等待，不能并发使用仅支持单个等待者的 pop_eventually。
    seastar::queue<pending_job> _pending;
    seastar::condition_variable _ready;
    std::vector<seastar::future<>> _consumers;

    void check_owner() const
    {
        if (seastar::this_shard_id() != _owner) {
            throw std::logic_error("cross-shard I/O submission");
        }
    }
    template <typename Operation> seastar::future<> execute(job<Operation> item)
    {
        try {
            if constexpr (std::is_void_v<typename Operation::result_type>) {
                co_await _backend.execute(std::move(item.operation));
                item.completion.set_value();
            } else {
                item.completion.set_value(co_await _backend.execute(std::move(item.operation)));
            }
        } catch (...) {
            item.completion.set_exception(std::current_exception());
        }
    }
    seastar::future<> consume()
    {
        while (true) {
            // 通知不预留任务；每次恢复后都重新检查条件，检查与 pop 之间不再挂起。
            // 使用协程原生 when() 等待，避免 wait(predicate) 的 future 续体引入调度间隙。
            while (_pending.empty() && _accepting) {
                co_await _ready.when();
            }
            if (!_accepting) {
                co_return;
            }
            // 字节额度只约束待执行项，取出后立即归还；在途操作数量由固定消费者数约束。
            // item 继续存活到 execute 完成，参数和 promise 不会因出队而失效。
            auto pending = _pending.pop();
            pending.bytes.return_all();
            auto item = std::move(pending.item);
            co_await std::visit([this]<typename Operation>(job<Operation>& value) { return execute(std::move(value)); },
                                *item);
        }
    }

  public:
    operation_queue(Backend& backend, queue_limits limits, seastar::scheduling_group group)
        : _backend(backend), _limits(limits), _group(group), _bytes(limits.bytes), _pending(limits.pending)
    {
        if (!limits.pending || !limits.bytes || !limits.concurrency) {
            throw std::invalid_argument("I/O queue limits must be positive");
        }
    }
    ~operation_queue() { assert(_consumers.empty() && _pending.empty()); }
    operation_queue(const operation_queue&) = delete;
    operation_queue& operator=(const operation_queue&) = delete;

    void start()
    {
        check_owner();
        if (_accepting || !_consumers.empty()) {
            throw std::logic_error("I/O queue already started");
        }
        _accepting = true;
        for (unsigned index = 0; index < _limits.concurrency; ++index) {
            _consumers.push_back(seastar::with_scheduling_group(_group, [this] { return consume(); }));
        }
    }
    // 成功准入后队列拥有参数；future 向调用者交付有类型的结果或异常，不自动重试。
    // 消费者在 I/O 组执行，但 co_await 此 future 的业务协程仍在原 shard、原调度组恢复。
    template <typename Operation>
        requires(std::same_as<Operation, Operations> || ...)
    seastar::future<typename Operation::result_type> submit(Operation operation)
    {
        check_owner();
        using result_type = typename Operation::result_type;
        if (!_accepting) {
            return seastar::make_exception_future<result_type>(stopped());
        }
        // entry 的固定存储与操作声明的动态参数大小合计用于额度估算，
        // 不包括分配器开销及驱动执行期间分配的缓冲区。
        const size_t bytes = sizeof(entry) + operation.byte_size();
        auto storage = seastar::try_get_units(_bytes, bytes);
        if (_pending.full() || !storage) {
            return seastar::make_exception_future<result_type>(overloaded());
        }
        auto item = std::make_unique<entry>(std::in_place_type<job<Operation>>, std::move(operation),
                                            seastar::promise<result_type>{});
        auto result = std::get<job<Operation>>(*item).completion.get_future();
        const bool pushed = _pending.push(pending_job{std::move(*storage), std::move(item)});
        assert(pushed);
        _ready.signal();
        return result;
    }
    // 关闭准入，给未执行项交付 stopped，并归还其额度；在途项仍由消费者持有。
    // 此函数本身不中止传输；所有者须中止后端 I/O，stop() 才能等到消费者全部退出。
    void begin_stop()
    {
        check_owner();
        _accepting = false;
        while (!_pending.empty()) {
            auto pending = _pending.pop();
            auto item = std::move(pending.item);
            std::visit([]<typename Operation>(job<Operation>& value) { value.completion.set_exception(stopped()); },
                       *item);
        }
        _ready.broadcast();
    }
    seastar::future<> stop()
    {
        begin_stop();
        co_await seastar::when_all_succeed(_consumers.begin(), _consumers.end());
        _consumers.clear();
    }
};
} // namespace imm::io
