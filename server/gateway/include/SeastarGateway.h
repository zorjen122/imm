#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/sharded.hh>
#include <seastar/net/api.hh>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace wimi
{
class SeastarExecutor;
}

namespace wimi::connection
{

class MessageLinkManager;
class SeastarGatewaySession;
class SessionManager;

class GatewayQueueBudget
{
    class Counter
    {
      public:
        Counter(std::size_t maxItems, std::size_t maxBytes) : maxItems(maxItems), maxBytes(maxBytes) {}

        bool TryAcquire(std::size_t bytes);
        bool TryResize(std::size_t oldBytes, std::size_t newBytes);
        void Release(std::size_t bytes);

      private:
        const std::size_t maxItems;
        const std::size_t maxBytes;
        std::atomic<std::size_t> items{};
        std::atomic<std::size_t> bytes{};
    };

  public:
    GatewayQueueBudget(std::size_t maxWriteItems, std::size_t maxWriteBytes, std::size_t maxReliableItems,
                       std::size_t maxReliableBytes)
        : writes(maxWriteItems, maxWriteBytes), reliable(maxReliableItems, maxReliableBytes)
    {
    }

    bool TryAcquireWrite(std::size_t bytes) { return writes.TryAcquire(bytes); }
    void ReleaseWrite(std::size_t bytes) { writes.Release(bytes); }
    bool TryAcquireReliable(std::size_t bytes) { return reliable.TryAcquire(bytes); }
    bool TryResizeReliable(std::size_t oldBytes, std::size_t newBytes)
    {
        return reliable.TryResize(oldBytes, newBytes);
    }
    void ReleaseReliable(std::size_t bytes) { reliable.Release(bytes); }

  private:
    Counter writes;
    Counter reliable;
};

class SeastarGatewayServer
{
  public:
    SeastarGatewayServer(SeastarExecutor& executor, SessionManager& sessions, MessageLinkManager& messageLinks,
                         GatewayQueueBudget& queueBudget, seastar::sharded<SeastarGatewayServer>* shards,
                         unsigned short port);

    seastar::future<> Start();
    seastar::future<> Stop();

    bool SendRaw(const std::string& connectionId, std::string envelope);
    bool SendReliable(const std::string& connectionId, std::string envelope, int64_t ackSeq);
    void Close(const std::string& connectionId);
    void DeliverCommandResult(const std::string& connectionId, std::string result, bool expectResponse, int64_t actor,
                              int operation, std::string requestId);

  private:
    friend class SeastarGatewaySession;

    seastar::future<> AcceptLoop();
    void Erase(const std::string& connectionId);
    std::shared_ptr<SeastarGatewaySession> Find(const std::string& connectionId);

    SeastarExecutor& executor;
    SessionManager& sessions;
    MessageLinkManager& messageLinks;
    GatewayQueueBudget& queueBudget;
    seastar::sharded<SeastarGatewayServer>* shards;
    unsigned short port;
    bool stopping{false};
    std::optional<seastar::server_socket> listener;
    seastar::future<> acceptTask = seastar::make_ready_future<>();
    std::unordered_map<std::string, std::shared_ptr<SeastarGatewaySession>> connections;
};

} // namespace wimi::connection
