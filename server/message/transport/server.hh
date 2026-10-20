#pragma once
#include "gateway_message.grpc.pb.h"
#include "io/options.hh"
#include "io/budget.hh"
#include "reactor/grpc_server.hh"
#include <seastar/core/gate.hh>
#include <seastar/core/scheduling.hh>
#include <functional>
#include <map>
#include <tuple>

namespace imm::message_transport
{
// token 区分同一 Gateway/shard 的重连；旧请求完成后只能回复发起它的那条流。
struct stream_route {
    std::string gateway_id;
    std::string instance_id;
    unsigned shard_id{};
    uint64_t token{};
};
class stream;
class server final : public gateway::GatewayMessageTransport::CallbackService
{
    using stream_key = std::tuple<std::string, std::string, unsigned>;
    message_io::options _options;
    seastar::scheduling_group _protocol;
    seastar::scheduling_group _io;
    io::grpc_server _server;
    message_io::budget _writes;
    seastar::gate _calls;
    seastar::gate _frames;
    std::map<uint64_t, std::shared_ptr<stream>> _streams;
    std::map<stream_key, uint64_t> _registered;
    uint64_t _next_token{};
    bool _stopping{};
    std::function<void(stream_route, gateway::CommandEnvelope)> _command;
    friend class stream;
    void register_stream(stream& source, const gateway::RegisterGateway& registration);
    void on_grpc_done(stream& source);
    seastar::future<> process(std::shared_ptr<stream> source, gateway::GatewayToMessageFrame frame);

  public:
    server(message_io::options config, seastar::scheduling_group protocol, seastar::scheduling_group io_group);
    ~server();
    void start(io::endpoint numeric_address, std::function<void(stream_route, gateway::CommandEnvelope)> command);
    grpc::ServerBidiReactor<gateway::GatewayToMessageFrame, gateway::MessageToGatewayFrame>*
    Connect(grpc::CallbackServerContext* context) override;
    bool reply(const stream_route& route, gateway::MessageToGatewayFrame frame);
    gateway::ClientForwardStatus forward(gateway::SessionLease lease, gateway::ClientForwardEnvelope envelope);
    uint16_t port() const { return _server.port(); }
    void begin_stop();
    seastar::future<> close();
};
} // namespace imm::message_transport
