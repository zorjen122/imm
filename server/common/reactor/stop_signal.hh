#pragma once
#include <seastar/core/condition-variable.hh>
#include <seastar/core/signal.hh>
#include <csignal>

namespace imm::io
{
// 在主 shard 持有到服务清理结束；SIGINT/SIGTERM 唤醒等待，由调用方关闭服务。
class stop_signal
{
    bool _stopping{};
    seastar::condition_variable _changed;

  public:
    void request_stop()
    {
        _stopping = true;
        _changed.broadcast();
    }

    stop_signal()
    {
        seastar::handle_signal(SIGINT, [this] { request_stop(); });
        seastar::handle_signal(SIGTERM, [this] { request_stop(); });
    }
    stop_signal(const stop_signal&) = delete;
    stop_signal& operator=(const stop_signal&) = delete;
    ~stop_signal()
    {
        // Seastar 没有移除 handler 的接口，替换为空回调，避免引用已销毁的对象。
        seastar::handle_signal(SIGINT, [] {});
        seastar::handle_signal(SIGTERM, [] {});
    }

    // 无超时等待退出信号；信号已到达时立即完成。
    seastar::future<> wait()
    {
        return _changed.wait([this] { return _stopping; });
    }
    bool stopping() const { return _stopping; }
};
} // namespace imm::io
