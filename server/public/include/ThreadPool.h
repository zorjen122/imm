#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace wimi
{

struct ThreadPoolOptions {
    std::size_t maxQueueSize{100000};
};

struct ThreadPoolStats {
    std::size_t workerCount{0};
    std::size_t queueSize{0};
    uint64_t submitted{0};
    uint64_t completed{0};
    uint64_t rejected{0};
};

class ThreadPool
{
  public:
    using Task = std::function<void()>;

    ThreadPool(std::string name, std::size_t workerCount, ThreadPoolOptions options = {});
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    bool Post(Task task);
    void Stop();
    ThreadPoolStats GetStats() const;

  private:
    void WorkerLoop(std::size_t index);
    bool PopTask(Task& task);

    std::string name;
    ThreadPoolOptions options;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::queue<Task> queue;
    std::vector<std::thread> workers;
    bool stopping{false};

    std::atomic<uint64_t> submitted{0};
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> rejected{0};
};

} // namespace wimi
