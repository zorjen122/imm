#include "Metrics.h"
#include "Mysql.h"
#include "Redis.h"
#include "RequestContext.h"
#include "RpcPool.h"
#include "ThreadPool.h"
#include "state.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace
{

using namespace std::chrono_literals;

bool Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool TestThreadPoolDeadline()
{
    wimi::ThreadPoolOptions options;
    options.maxQueueSize = 1;
    wimi::ThreadPool pool("deadline-test", 1, options);
    std::promise<void> release;
    auto releaseFuture = release.get_future().share();
    std::promise<void> started;

    bool first = pool.Post([&]() {
        started.set_value();
        releaseFuture.wait();
    });
    started.get_future().wait();
    bool second = pool.Post([&]() { releaseFuture.wait(); });
    auto before = std::chrono::steady_clock::now();
    auto status = pool.PostUntil([]() {}, before + 40ms, before + 1s);
    auto elapsed = std::chrono::steady_clock::now() - before;
    release.set_value();
    pool.Stop();

    return Require(first && second, "failed to fill thread pool") &&
           Require(status == wimi::ThreadPool::PostStatus::TimedOut, "full thread pool did not time out") &&
           Require(elapsed >= 30ms && elapsed < 500ms, "thread pool acquire ignored its deadline") &&
           Require(pool.GetStats().acquireTimeouts == 1, "thread pool timeout metric mismatch");
}

bool TestMysqlPoolDeadline()
{
    wimi::db::MysqlPool pool("127.0.0.1", 33060, "zorjen", "root", "chatServ", 1);
    auto held = pool.GetConnectionUntil(wimi::RequestContext::Clock::now() + 1s);
    if (!Require(held != nullptr, "unable to acquire MySQL test connection")) {
        return false;
    }
    auto before = wimi::RequestContext::Clock::now();
    auto blocked = pool.GetConnectionUntil(before + 40ms);
    bool ok = Require(blocked == nullptr, "exhausted MySQL pool did not time out") &&
              Require(wimi::RequestContext::Clock::now() - before < 500ms, "MySQL pool ignored its deadline");
    pool.ReturnConnection(std::move(held));
    return ok;
}

bool TestRedisPoolDeadline()
{
    sw::redis::SentinelOptions options;
    options.nodes.emplace_back("127.0.0.1", 26389);
    options.nodes.emplace_back("127.0.0.1", 26390);
    options.nodes.emplace_back("127.0.0.1", 26391);
    options.password = "root";
    auto sentinel = std::make_shared<sw::redis::Sentinel>(options);
    wimi::db::RedisPool pool(std::move(sentinel), "wimi-master", "root", 1);
    auto held = pool.GetConnectionUntil(wimi::RequestContext::Clock::now() + 1s);
    if (!Require(held != nullptr, "unable to acquire Redis test connection")) {
        return false;
    }
    auto before = wimi::RequestContext::Clock::now();
    auto blocked = pool.GetConnectionUntil(before + 40ms);
    bool ok = Require(blocked == nullptr, "exhausted Redis pool did not time out") &&
              Require(wimi::RequestContext::Clock::now() - before < 500ms, "Redis pool ignored its deadline");
    pool.ReturnConnection(std::move(held));
    return ok;
}

bool TestRpcPoolDeadline()
{
    RpcPool<state::StateService> pool(1, "127.0.0.1", "50055");
    auto held = pool.getConnectionUntil(wimi::RequestContext::Clock::now() + 1s);
    if (!Require(held != nullptr, "unable to acquire RPC test connection")) {
        return false;
    }
    auto before = wimi::RequestContext::Clock::now();
    auto blocked = pool.getConnectionUntil(before + 40ms);
    bool ok = Require(blocked == nullptr, "exhausted RPC pool did not time out") &&
              Require(wimi::RequestContext::Clock::now() - before < 500ms, "RPC pool ignored its deadline");
    pool.returnConnection(std::move(held));
    return ok;
}

} // namespace

int main()
{
    bool ok = TestThreadPoolDeadline();
    if (std::getenv("WIMI_RUN_POOL_INTEGRATION_TESTS") != nullptr) {
        ok = TestMysqlPoolDeadline() && TestRedisPoolDeadline() && TestRpcPoolDeadline() && ok;
    }
    return ok ? 0 : 1;
}
