#include "IoQueue.h"
#include <seastar/core/app-template.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/later.hh>
#include <deque>
#include <string>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
struct held_operation {
    using result_type = std::string;
    std::string value;
    size_t byte_size() const { return value.size(); }
};
struct immediate_operation {
    using result_type = void;
    size_t byte_size() const { return 0; }
};
struct backend {
    unsigned owner{seastar::this_shard_id()};
    seastar::scheduling_group group;
    unsigned started{};
    std::deque<seastar::promise<>> pending;
    seastar::future<std::string> execute(held_operation operation)
    {
        require(owner == seastar::this_shard_id(), "backend moved to another shard");
        require(seastar::current_scheduling_group() == group, "backend outside I/O group");
        ++started;
        pending.emplace_back();
        co_await pending.back().get_future();
        co_return operation.value;
    }
    seastar::future<> execute(immediate_operation)
    {
        require(seastar::current_scheduling_group() == group, "second operation dispatch outside I/O group");
        co_return;
    }
    void release()
    {
        pending.front().set_value();
        pending.pop_front();
    }
};
template <typename Exception, typename Result> seastar::future<> expect_exception(seastar::future<Result> result)
{
    bool caught{};
    try {
        co_await std::move(result);
    } catch (const Exception&) {
        caught = true;
    }
    require(caught, "missing expected queue error");
}
seastar::future<> test_shard(seastar::scheduling_group group)
{
    const auto caller = seastar::current_scheduling_group();
    backend driver{.group = group};
    imm::io::operation_queue<backend, std::variant<held_operation, immediate_operation>> queue(driver, {1, 4096, 1},
                                                                                               group);
    queue.start();
    auto first = queue.submit(held_operation{"one"});
    while (driver.started < 1) {
        co_await seastar::yield();
    }
    auto second = queue.submit(held_operation{"two"});
    co_await expect_exception<imm::io::overloaded>(queue.submit(held_operation{"rejected"}));
    driver.release();
    require(co_await std::move(first) == "one", "first result misdelivered");
    require(seastar::current_scheduling_group() == caller, "request resumed in I/O scheduling group");
    while (driver.started < 2) {
        co_await seastar::yield();
    }
    auto queued = queue.submit(held_operation{"never executed"});
    queue.begin_stop();
    co_await expect_exception<imm::io::stopped>(std::move(queued));
    co_await expect_exception<imm::io::stopped>(queue.submit(immediate_operation{}));
    driver.release();
    require(co_await std::move(second) == "two", "in-flight job lost during stop");
    co_await queue.stop();
    require(driver.started == 2, "rejected job executed");

    imm::io::operation_queue<backend, std::variant<held_operation>> byte_queue(driver, {8, 1, 1}, group);
    byte_queue.start();
    co_await expect_exception<imm::io::overloaded>(byte_queue.submit(held_operation{"too large"}));
    co_await byte_queue.stop();

    imm::io::operation_queue<backend, std::variant<immediate_operation>> typed_queue(driver, {1, 1024, 1}, group);
    typed_queue.start();
    co_await typed_queue.submit(immediate_operation{});
    co_await typed_queue.stop();
    // 多消费者已被唤醒，但任务可能被先运行的消费者全部取走；通知本身不预留队列项。
    // 连续批量提交立即完成的操作，复现旧 wait(predicate) 在恢复间隙丢失条件的空队列 pop。
    imm::io::operation_queue<backend, std::variant<immediate_operation>> fast_queue(driver, {64, 16384, 16}, group);
    fast_queue.start();
    for (unsigned round = 0; round < 512; ++round) {
        std::vector<seastar::future<>> results;
        for (unsigned index = 0; index < 32; ++index) {
            results.push_back(fast_queue.submit(immediate_operation{}));
        }
        co_await seastar::when_all_succeed(results.begin(), results.end());
    }
    co_await fast_queue.stop();
}
seastar::future<int> run()
{
    const auto group = co_await seastar::create_scheduling_group("io", 200);
    auto local = test_shard(group);
    auto remote = seastar::smp::submit_to(1, [group] { return test_shard(group); });
    co_await std::move(local);
    co_await std::move(remote);
    co_await seastar::destroy_scheduling_group(group);
    co_return 0;
}
} // namespace
int main(int argc, char** argv)
{
    seastar::app_template app;
    return app.run(argc, argv, run);
}
