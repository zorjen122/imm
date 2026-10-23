#pragma once
#include "options.hh"
#include "reactor/dns_resolver.hh"
#include "reactor/grpc_server.hh"
#include "state.grpc.pb.h"
#include <seastar/core/gate.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/semaphore.hh>
#include <functional>

namespace imm::state_service
{
struct scheduling_groups {
    seastar::scheduling_group protocol;
    seastar::scheduling_group transaction;
    seastar::scheduling_group io;
};
class unary_call;
// 每个 shard 独占 listener、gRPC Server、拓扑快照及轮询游标，无共享连接或原子计数。
class service final : public ::state::StateService::CallbackService
{
    const options _options;
    const scheduling_groups _groups;
    io::dns_resolver _dns;
    io::grpc_server _server;
    seastar::semaphore _capacity;
    seastar::gate _calls;
    seastar::gate _tasks;
    std::size_t _next_gateway{};
    bool _stopping{};
    bool _stopped{};
    seastar::future<> execute(unary_call* call, std::function<grpc::Status()> operation,
                              seastar::gate::holder lifetime);
    grpc::ServerUnaryReactor* submit(grpc::CallbackServerContext& context, std::function<grpc::Status()> operation);

  public:
    service(options config, scheduling_groups groups);
    seastar::future<> start();
    seastar::future<> stop();
    grpc::ServerUnaryReactor* PickConnectionGateway(grpc::CallbackServerContext*, const ::state::ConnectUser*,
                                                    ::state::ConnectUserRsp* response) override;
    grpc::ServerUnaryReactor* ListMessageNodes(grpc::CallbackServerContext*, const ::state::TopologyRequest*,
                                               ::state::MessageTopology* response) override;
};
} // namespace imm::state_service
