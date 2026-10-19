#pragma once
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/when_all.hh>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <system_error>
#include <vector>

namespace wimi::io
{
// 原 socket 由协议库拥有；dup 出的描述符只交给 reactor 监控就绪，不负责协议读写。
// close 副本可撤销监控且不关闭原描述符；shutdown 则会影响同一个 socket，不能用于此目的。
class fd_watch : public std::enable_shared_from_this<fd_watch>
{
    seastar::pollable_fd _fd;
    seastar::condition_variable _changed[2];
    bool _enabled[2]{};
    bool _closing{};
    std::function<void()> _callbacks[2];
    std::vector<seastar::future<>> _tasks;
    static seastar::pollable_fd duplicate(int fd)
    {
        const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (copy < 0) {
            throw std::system_error(errno, std::system_category(), "duplicate readiness socket");
        }
        return seastar::pollable_fd(seastar::file_desc::from_fd(copy));
    }
    fd_watch(int fd, std::function<void()> read, std::function<void()> write)
        : _fd(duplicate(fd)), _callbacks{std::move(read), std::move(write)}
    {
    }
    // 按值持有 shared_ptr，保证监控协程跨挂起点及回调期间仍可访问对象。
    // 所有者必须调用并等待 stop()，让两个任务退出并释放这一持有关系。
    static seastar::future<> run(std::shared_ptr<fd_watch> self, unsigned index)
    {
        while (!self->_closing) {
            co_await self->_changed[index].wait([&] { return self->_closing || self->_enabled[index]; });
            if (self->_closing) {
                break;
            }
            try {
                if (index == 0) {
                    co_await self->_fd.readable();
                } else {
                    co_await self->_fd.writeable();
                }
            } catch (...) {
                if (self->_closing) {
                    break;
                }
                throw;
            }
            if (!self->_closing && self->_enabled[index]) {
                self->_callbacks[index]();
            }
        }
    }

  public:
    static std::shared_ptr<fd_watch> create(int fd, std::function<void()> read, std::function<void()> write)
    {
        auto watch = std::shared_ptr<fd_watch>(new fd_watch(fd, std::move(read), std::move(write)));
        watch->_tasks.push_back(run(watch, 0));
        watch->_tasks.push_back(run(watch, 1));
        return watch;
    }
    void set(unsigned index, bool enabled)
    {
        _enabled[index] = enabled;
        _changed[index].broadcast();
    }
    seastar::future<> stop()
    {
        _closing = true;
        _changed[0].broadcast();
        _changed[1].broadcast();
        // broadcast 唤醒尚未启用的方向，close 唤醒已在等待 fd 就绪的方向；
        // 两类等待全部结束后才能销毁捕获了协议库状态的回调。
        _fd.close();
        co_await seastar::when_all_succeed(_tasks.begin(), _tasks.end());
        _tasks.clear();
    }
};

} // namespace wimi::io
