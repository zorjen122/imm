#include "IocPool.h"
#include "Logger.h"
#include "spdlog/spdlog.h"

namespace wimi
{
IocPool::IocPool(std::size_t size)
    : iocGroup(size), worker(size), threadPool(std::make_unique<ThreadPool>("io-loop", size))
{
    for (std::size_t i = 0; i < size; ++i)
        worker[i] = std::make_unique<Work>(iocGroup[i].get_executor());

    for (std::size_t i = 0; i < iocGroup.size(); ++i) {
        threadPool->Post([this, i]() { iocGroup[i].run(); });
    }

    if (iocGroup.size() >= 1)
        LOG_INFO(logger, "IocPool created with {} threads", iocGroup.size());
    else
        LOG_WARN(logger, "IocPool created is failed, size {} ", iocGroup.size());
}

IocPool::~IocPool()
{
    spdlog::info("ServicePool::~ServicePool");
    Stop();
}

boost::asio::io_context& IocPool::GetContext()
{
    static size_t currentIoc = 0;
    auto& ioc = iocGroup[currentIoc++];
    currentIoc = currentIoc % iocGroup.size();

    return ioc;
}

void IocPool::Stop()
{
    for (auto& ioc : iocGroup)
        ioc.stop();

    for (auto& work : worker)
        work.reset();

    if (threadPool) {
        threadPool->Stop();
    }
}
}; // namespace wimi
