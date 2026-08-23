#include "ThreadPool.h"

#include "Logger.h"

#include <algorithm>
#include <exception>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wimi
{
namespace
{

void SetCurrentThreadName(const std::string& poolName, std::size_t index)
{
    std::string threadName = poolName + "-" + std::to_string(index);
    if (threadName.size() > 15) {
        threadName.resize(15);
    }

#if defined(__linux__)
    pthread_setname_np(pthread_self(), threadName.c_str());
#elif defined(__APPLE__)
    pthread_setname_np(threadName.c_str());
#elif defined(_WIN32)
    std::wstring wideName(threadName.begin(), threadName.end());
    HMODULE kernel = GetModuleHandleW(L"Kernel32.dll");
    if (kernel == nullptr) {
        return;
    }

    using SetThreadDescriptionFunc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    auto setThreadDescription =
        reinterpret_cast<SetThreadDescriptionFunc>(GetProcAddress(kernel, "SetThreadDescription"));
    if (setThreadDescription != nullptr) {
        setThreadDescription(GetCurrentThread(), wideName.c_str());
    }
#else
    (void)threadName;
#endif
}

} // namespace

ThreadPool::ThreadPool(std::string name, std::size_t workerCount, ThreadPoolOptions options)
    : name(std::move(name)), options(options)
{
    workerCount = std::max<std::size_t>(1, workerCount);
    workers.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i) {
        workers.emplace_back([this, i]() { WorkerLoop(i); });
    }
    LOG_INFO(logger, "ThreadPool {} started with {} workers", this->name, workerCount);
}

ThreadPool::~ThreadPool() { Stop(); }

bool ThreadPool::Post(Task task)
{
    if (!task) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || queue.size() >= options.maxQueueSize) {
            if (!stopping) {
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
        queue.push(std::move(task));
        submitted.fetch_add(1, std::memory_order_relaxed);
    }

    condition.notify_one();
    return true;
}

void ThreadPool::Stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) {
            return;
        }
        stopping = true;
    }

    condition.notify_all();
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers.clear();
}

ThreadPoolStats ThreadPool::GetStats() const
{
    ThreadPoolStats stats;
    stats.workerCount = workers.size();
    {
        std::lock_guard<std::mutex> lock(mutex);
        stats.queueSize = queue.size();
    }
    stats.submitted = submitted.load(std::memory_order_relaxed);
    stats.completed = completed.load(std::memory_order_relaxed);
    stats.rejected = rejected.load(std::memory_order_relaxed);
    return stats;
}

void ThreadPool::WorkerLoop(std::size_t index)
{
    SetCurrentThreadName(name, index);

    for (;;) {
        Task task;
        if (!PopTask(task)) {
            return;
        }

        try {
            task();
        } catch (const std::exception& error) {
            LOG_ERROR(logger, "ThreadPool {} task failed: {}", name, error.what());
        } catch (...) {
            LOG_ERROR(logger, "ThreadPool {} task failed: unknown exception", name);
        }

        completed.fetch_add(1, std::memory_order_relaxed);
    }
}

bool ThreadPool::PopTask(Task& task)
{
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [this]() { return stopping || !queue.empty(); });

    if (stopping && queue.empty()) {
        return false;
    }

    task = std::move(queue.front());
    queue.pop();
    return true;
}

} // namespace wimi
