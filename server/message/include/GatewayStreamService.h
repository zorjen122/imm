#pragma once

#include "gateway_message.grpc.pb.h"

#include <grpcpp/support/server_callback.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <map>
#include <tuple>
#include <condition_variable>
#include <functional>

namespace wimi::db
{
struct SessionLease;
}

namespace wimi::rpc
{

class GatewayStreamReactor;

class GatewayStreamService final : public gateway::GatewayMessageTransport::CallbackService
{
  public:
    explicit GatewayStreamService(std::string messageNodeId, bool requirePeerIdentity = false);

    grpc::ServerBidiReactor<gateway::GatewayToMessageFrame, gateway::MessageToGatewayFrame>*
    Connect(grpc::CallbackServerContext* context) override;

    void Register(const std::string& gatewayId, const std::string& instanceId, unsigned shardId, uint64_t streamToken,
                  GatewayStreamReactor* reactor);
    void Unregister(const std::string& gatewayId, const std::string& instanceId, unsigned shardId, uint64_t streamToken,
                    GatewayStreamReactor* reactor);
    bool Forward(const db::SessionLease& lease, gateway::ClientForwardEnvelope envelope);
    bool ForwardToUser(int64_t recipientUid, gateway::ClientForwardEnvelope envelope,
                       const std::string& excludedDeviceId = {});
    bool Reply(const std::string& gatewayId, const std::string& instanceId, unsigned shardId, uint64_t streamToken,
               gateway::MessageToGatewayFrame frame);
    bool post_command(std::function<void()> task);
    void wait_commands();
    void DrainAll(const std::string& reason);
    const std::string& MessageNodeId() const;
    bool RequirePeerIdentity() const;
    bool Draining() const;

  private:
    struct RegisteredStream {
        std::string instanceId;
        GatewayStreamReactor* reactor{nullptr};
        uint64_t token{};
    };

    void command_complete();
    std::mutex _commands_mutex;
    std::condition_variable _commands_changed;
    size_t _commands{};
    std::string messageNodeId;
    bool requirePeerIdentity{false};
    std::atomic<bool> draining{false};
    std::mutex streamsMutex;
    std::map<std::tuple<std::string, std::string, unsigned>, RegisteredStream> streams;
};

} // namespace wimi::rpc
