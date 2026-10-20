#pragma once
#include "global.h"
#include "state.grpc.pb.h"
#include "state.pb.h"

#include <atomic>

namespace imm::rpc
{
using state::ConnectUser;
using state::ConnectUserRsp;
using state::MessageTopology;
using state::StateService;
using state::TopologyRequest;

class StateServiceImpl final : public StateService::Service
{
  public:
    StateServiceImpl();
    grpc::Status PickConnectionGateway(grpc::ServerContext* context, const ConnectUser* request,
                                       ConnectUserRsp* response) override;

    grpc::Status ListMessageNodes(grpc::ServerContext* context, const TopologyRequest* request,
                                  MessageTopology* response) override;

    std::vector<ServiceNodeInfo> gatewayNodes;
    std::vector<ServiceNodeInfo> messageNodes;
    std::atomic<std::size_t> gatewayRouteCount{0};
};
}; // namespace imm::rpc
