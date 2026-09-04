#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace wimi
{

namespace rpc
{
class GatewayStreamService;
}

class ClientForwardService
{
  public:
    void SetGatewayStreamService(rpc::GatewayStreamService* service);
    bool ForwardToGateway(int64_t uid, const std::string& packet, int64_t forwardId = 0, int64_t messageId = 0,
                          int64_t conversationId = 0, int64_t conversationSeq = 0) const;

  private:
    std::atomic<rpc::GatewayStreamService*> gatewayStreamService{nullptr};
};

} // namespace wimi
