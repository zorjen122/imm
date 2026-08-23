#pragma once

#include "MessageLink.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <grpcpp/security/credentials.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wimi::connection
{

class MessageLinkManager
{
  public:
    using CommandCallback = std::function<void(const gateway::CommandResult& result)>;
    using ClientForwardHandler =
        std::function<gateway::ClientForwardStatus(const gateway::ClientForwardEnvelope& forward)>;
    using Node = MessageLink::Node;

    MessageLinkManager(boost::asio::io_context& ioContext, boost::asio::thread_pool& controlPool, std::string gatewayId,
                       std::string instanceId);
    ~MessageLinkManager();

    void Start();
    void Stop();
    bool Ready() const;
    std::size_t HealthyLinkCount() const;
    boost::asio::awaitable<bool> Forward(gateway::CommandEnvelope command, CommandCallback callback);
    void SetClientForwardHandler(ClientForwardHandler handler);

  private:
    friend class MessageLink;

    struct PendingCommand {
        gateway::CommandEnvelope command;
        CommandCallback callback;
        // 单次投递 deadline：到期后忽略当前链路实例并尝试其他 State 链路。
        std::shared_ptr<boost::asio::steady_timer> attemptTimer;
        std::unordered_set<std::string> ignoredLinkTokens;
        std::string currentLinkToken;
        std::uint64_t attempt{0};
        bool retryPending{false};
        bool retrying{false};
    };
    struct TopologySnapshot {
        bool received{false};
        std::vector<Node> nodes;
    };

    boost::asio::awaitable<void> TopologyLoop();
    TopologySnapshot FetchTopology();
    void SyncTopologyLink(const TopologySnapshot& snapshot);
    void StartLink(const Node& node);
    void OnFrame(const std::string& nodeId, const gateway::MessageToGatewayFrame& frame);
    void OnLinkDone(const std::string& nodeId, MessageLink* source);
    void OnLinkWritable();
    void RetryPending(const std::string& failedLinkToken = {});
    void OnAttemptTimeout(const std::string& requestId, std::uint64_t attempt);
    void ArmAttemptTimer(const std::string& requestId, std::uint64_t attempt);
    void AppendPendingDone(std::shared_ptr<MessageLink> link);
    std::shared_ptr<MessageLink> SelectLink(int64_t conversationId,
                                            const std::unordered_set<std::string>& ignoredLinkTokens = {});

    boost::asio::io_context& ioContext;
    boost::asio::thread_pool& controlPool;
    std::string gatewayId;
    std::string instanceId;
    std::string stateAddress;
    std::shared_ptr<grpc::ChannelCredentials> messageCredentials;
    std::atomic<bool> stopping{false};

    mutable std::mutex linksMutex;
    std::unordered_map<std::string, Node> topologyNodes;
    std::unordered_map<std::string, std::shared_ptr<MessageLink>> links;
    std::vector<std::shared_ptr<MessageLink>> donePending;
    std::atomic<std::uint64_t> nextLinkToken{0};

    std::mutex pendingMutex;
    std::unordered_map<std::string, PendingCommand> pending;
    ClientForwardHandler clientForwardHandler;
};

} // namespace wimi::connection
