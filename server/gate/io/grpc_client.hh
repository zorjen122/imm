#pragma once
#include "io/operations.hh"
#include <grpc/event_engine/event_engine.h>
#include <grpcpp/generic/generic_stub.h>
#include <seastar/core/future.hh>
#include <memory>
#include <set>
#include <string>

namespace wimi::gate_io
{
class grpc_client
{
    std::shared_ptr<grpc_event_engine::experimental::EventEngine> _engine;
    std::shared_ptr<grpc::Channel> _channel;
    std::unique_ptr<grpc::GenericStub> _stub;
    std::set<grpc::ClientContext*> _calls;

  public:
    explicit grpc_client(std::string target);
    seastar::future<gateway_node> pick_gateway(int64_t uid);
    void abort();
    seastar::future<> close();
};
} // namespace wimi::gate_io
