#pragma once
#include "io/budget.hh"
#include "io/options.hh"
#include "io/types.hh"
#include "gateway_message.grpc.pb.h"
#include "reactor/grpc_engine.hh"
#include <seastar/core/gate.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/scheduling.hh>

namespace wimi::gateway_io
{
class driver;
// 流在所属 shard 的 io 调度组构造。gRPC 回调和协议事件都由同一个 reactor 驱动，
// 发送协程持有帧及预算，在 io 调度组等待唯一写槽位，并保留帧到 OnWriteDone。
class message_link final
    : public grpc::ClientBidiReactor<gateway::GatewayToMessageFrame, gateway::MessageToGatewayFrame>
{
    driver& _driver;
    options _options;
    message_node_id _node_id;
    io::endpoint _address;
    unsigned _owner{seastar::this_shard_id()};
    std::shared_ptr<grpc_event_engine::experimental::EventEngine> _engine;
    std::shared_ptr<grpc::Channel> _channel;
    std::unique_ptr<gateway::GatewayMessageTransport::Stub> _stub;
    std::unique_ptr<grpc::ClientContext> _context{std::make_unique<grpc::ClientContext>()};
    gateway::MessageToGatewayFrame _read;
    seastar::scheduling_group _io_group{seastar::current_scheduling_group()};
    seastar::semaphore _write_slot{1};
    seastar::gate _writers;
    seastar::promise<bool>* _write_done{};
    seastar::promise<> _done;
    bool _stopping{};
    bool _finished{};
    bool _healthy{};
    size_t _inflight{};
    seastar::future<> write(gateway::GatewayToMessageFrame frame, capacity_units units,
                            seastar::future<seastar::semaphore_units<>> slot);
    void check_owner() const;

  public:
    message_link(driver& owner, options config, message_node_id node_id, io::endpoint address);
    ~message_link();
    void start();
    void enqueue(gateway::GatewayToMessageFrame frame);
    void stop();
    seastar::future<> close();
    bool healthy() const { return _healthy && !_stopping; }
    bool finished() const { return _finished; }
    const io::endpoint& address() const { return _address; }
    const message_node_id& node_id() const { return _node_id; }
    size_t inflight() const { return _inflight; }
    void add_inflight() { ++_inflight; }
    void remove_inflight()
    {
        if (_inflight) {
            --_inflight;
        }
    }
    void OnReadDone(bool ok) override;
    void OnWriteDone(bool ok) override;
    void OnDone(const grpc::Status& status) override;
};
} // namespace wimi::gateway_io
