#pragma once

#include <seastar/core/alien.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace wimi
{

class ExecutorOverloaded : public std::runtime_error
{
  public:
    ExecutorOverloaded() : std::runtime_error("blocking executor capacity exhausted") {}
};

class SeastarExecutor
{
    struct TaskBase {
        virtual ~TaskBase() = default;
        virtual void Execute() noexcept = 0;
        virtual void Complete() noexcept = 0;

        seastar::alien::instance* alien{};
        unsigned shard{};
        std::size_t bytes{};
    };

    template <typename Result> struct Task final : TaskBase {
        explicit Task(std::function<Result()> function) : function(std::move(function)) {}

        void Execute() noexcept override
        {
            try {
                result.emplace(function());
            } catch (...) {
                error = std::current_exception();
            }
        }

        void Complete() noexcept override
        {
            if (error)
                completion.set_exception(error);
            else
                completion.set_value(std::move(*result));
        }

        std::function<Result()> function;
        std::optional<Result> result;
        std::exception_ptr error;
        seastar::promise<Result> completion;
    };

  public:
    SeastarExecutor(std::size_t workerCount, std::size_t maxTasks, std::size_t maxBytes)
        : maxTasks(maxTasks), maxBytes(maxBytes)
    {
        if (workerCount == 0 || maxTasks == 0 || maxBytes == 0)
            throw std::invalid_argument("executor limits must be positive");

        workers.reserve(workerCount);
        for (std::size_t i = 0; i < workerCount; ++i)
            workers.emplace_back([this] { WorkerLoop(); });
    }

    SeastarExecutor(const SeastarExecutor&) = delete;
    SeastarExecutor& operator=(const SeastarExecutor&) = delete;

    ~SeastarExecutor()
    {
        {
            std::lock_guard lock(mutex);
            accepting = false;
            stopping = true;
        }
        wake.notify_all();
        workers.clear();
    }

    template <typename Function>
    auto Submit(std::size_t bytes, Function&& function) -> seastar::future<std::invoke_result_t<std::decay_t<Function>>>
    {
        using Result = std::invoke_result_t<std::decay_t<Function>>;
        static_assert(!std::is_void_v<Result>);

        std::function<Result()> callable(std::forward<Function>(function));
        std::unique_lock lock(mutex);
        if (!accepting || admittedTasks >= maxTasks || bytes > maxBytes - admittedBytes)
            throw ExecutorOverloaded();

        auto task = std::make_shared<Task<Result>>(std::move(callable));
        task->alien = &seastar::engine().alien();
        task->shard = seastar::this_shard_id();
        task->bytes = bytes;
        auto future = task->completion.get_future();
        queue.push_back(task);
        ++admittedTasks;
        admittedBytes += bytes;
        admitted.store(admittedTasks, std::memory_order_release);
        lock.unlock();

        wake.notify_one();
        return future;
    }

    seastar::future<> Stop()
    {
        {
            std::lock_guard lock(mutex);
            accepting = false;
        }
        while (admitted.load(std::memory_order_acquire) != 0)
            co_await seastar::sleep(std::chrono::milliseconds(1));
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        workers.clear();
    }

    std::size_t AdmittedTasks() const { return admitted.load(std::memory_order_relaxed); }

  private:
    void WorkerLoop()
    {
        while (true) {
            std::shared_ptr<TaskBase> task;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stopping || !queue.empty(); });
                if (queue.empty())
                    return;
                task = std::move(queue.front());
                queue.pop_front();
            }

            task->Execute();
            auto* alien = task->alien;
            const auto shard = task->shard;
            seastar::alien::run_on(*alien, shard, [this, task = std::move(task)]() noexcept {
                task->Complete();
                {
                    std::lock_guard lock(mutex);
                    --admittedTasks;
                    admittedBytes -= task->bytes;
                    admitted.store(admittedTasks, std::memory_order_release);
                }
            });
        }
    }

    const std::size_t maxTasks;
    const std::size_t maxBytes;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::shared_ptr<TaskBase>> queue;
    std::vector<std::jthread> workers;
    std::size_t admittedTasks{};
    std::size_t admittedBytes{};
    std::atomic<std::size_t> admitted{0};
    bool accepting{true};
    bool stopping{false};
};

} // namespace wimi
