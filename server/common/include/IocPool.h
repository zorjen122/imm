#pragma once

#include "Const.h"
#include "ThreadPool.h"

#include <boost/asio.hpp>
#include <memory>
#include <vector>

namespace wimi
{
class IocPool : public Singleton<IocPool>
{
    friend Singleton<IocPool>;

  public:
    using Service = boost::asio::io_context;
    using Work = boost::asio::executor_work_guard<Service::executor_type>;
    using WorkPtr = std::unique_ptr<Work>;
    ~IocPool();
    IocPool(const IocPool&) = delete;
    IocPool& operator=(const IocPool&) = delete;
    boost::asio::io_context& GetContext();
    void Stop();

  private:
    IocPool(std::size_t size = 2);
    std::vector<Service> iocGroup;
    std::vector<WorkPtr> worker;
    std::unique_ptr<ThreadPool> threadPool;
};
}; // namespace wimi
