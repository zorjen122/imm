#include "io/grpc_client.hh"
#include "reactor/grpc_engine.hh"
#include "state.pb.h"
#include <grpcpp/grpcpp.h>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/task.hh>
#include <seastar/util/defer.hh>
#include <seastar/core/sleep.hh>

namespace imm::gate_io
{
namespace ee = grpc_event_engine::experimental;
grpc_client::grpc_client(std::string target) : _engine(io::make_grpc_engine())
{
    grpc::ChannelArguments arguments;
    arguments.SetInt(GRPC_ARG_ENABLE_HTTP_PROXY, 0);
    arguments.SetInt(GRPC_ARG_SERVICE_CONFIG_DISABLE_RESOLUTION, 1);
    arguments.SetPointerWithVtable(GRPC_ARG_EVENT_ENGINE, &_engine, ee::grpc_event_engine_arg_vtable());
    _channel = grpc::CreateCustomChannel("dns:///" + target, grpc::InsecureChannelCredentials(), arguments);
    _stub = std::make_unique<grpc::GenericStub>(_channel);
}
seastar::future<gateway_node> grpc_client::pick_gateway(int64_t uid)
{
    state::ConnectUser source;
    source.set_id(uid);
    const std::string encoded = source.SerializeAsString();
    grpc::Slice slice(encoded);
    grpc::ByteBuffer request(&slice, 1), response;
    grpc::ClientContext context;
    _calls.insert(&context);
    auto remove = seastar::defer([this, &context]() noexcept { _calls.erase(&context); });
    seastar::promise<grpc::Status> completed;
    auto result = completed.get_future();
    const unsigned owner = seastar::this_shard_id();
    _stub->UnaryCall(&context, "/state.StateService/PickConnectionGateway", {}, &request, &response,
                     [&completed, owner](grpc::Status status) {
                         if (!seastar::engine_is_ready() || seastar::this_shard_id() != owner) {
                             std::terminate();
                         }
                         completed.set_value(std::move(status));
                     });
    const auto status = co_await std::move(result);
    if (!status.ok()) {
        throw std::runtime_error("State RPC failed: " + status.error_message());
    }
    std::vector<grpc::Slice> slices;
    if (!response.Dump(&slices).ok()) {
        throw std::runtime_error("invalid State RPC response");
    }
    std::string bytes;
    for (const grpc::Slice& part : slices) {
        bytes.append(reinterpret_cast<const char*>(part.begin()), part.size());
    }
    state::ConnectUserRsp message;
    if (!message.ParseFromString(bytes) || message.ip().empty() || message.port() <= 0 || message.port() > 65535) {
        throw std::runtime_error("State RPC returned no gateway");
    }
    co_return gateway_node{message.ip(), static_cast<uint16_t>(message.port()), message.node_id()};
}
void grpc_client::abort()
{
    for (grpc::ClientContext* context : _calls) {
        context->TryCancel();
    }
}
seastar::future<> grpc_client::close()
{
    if (!_calls.empty()) {
        throw std::logic_error("State RPC still running during close");
    }
    _stub.reset();
    _channel.reset();
    co_await io::close_grpc_engine(_engine);
    _engine.reset();
    co_await io::drain_local_grpc();
}
} // namespace imm::gate_io
