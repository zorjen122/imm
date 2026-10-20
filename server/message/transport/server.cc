#include "transport/server.hh"
#include "message_route.hh"
#include "Logger.h"
#include <seastar/core/coroutine.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <cassert>
#include <algorithm>

namespace imm::message_transport
{
class stream final : public grpc::ServerBidiReactor<gateway::GatewayToMessageFrame, gateway::MessageToGatewayFrame>,
                     public std::enable_shared_from_this<stream>
{
    server& _server;
    grpc::CallbackServerContext* _context;
    seastar::gate::holder _lifetime;
    gateway::GatewayToMessageFrame _read;
    seastar::semaphore _write_slot{1};
    seastar::promise<bool>* _write_done{};
    stream_route _route;
    std::vector<std::string> _peer_identities;
    unsigned _owner{seastar::this_shard_id()};
    bool _registered{};
    bool _stopping{};
    bool _finishing{};
    bool _finished{};
    friend class server;
    void check_owner() const { assert(seastar::engine_is_ready() && seastar::this_shard_id() == _owner); }
    void finish()
    {
        // Finish 与 StartWrite 不重叠；正在写的帧仍由发送协程持有至 OnWriteDone。
        if (!_finishing && !_finished && !_write_done) {
            _finishing = true;
            Finish(grpc::Status::OK);
        }
    }
    static seastar::future<> write(std::shared_ptr<stream> self, gateway::MessageToGatewayFrame frame,
                                   message_io::capacity_units units, seastar::future<seastar::semaphore_units<>> slot)
    {
        auto exclusive = co_await std::move(slot);
        if (self->_stopping) {
            co_return;
        }
        seastar::promise<bool> complete;
        auto result = complete.get_future();
        self->_write_done = &complete;
        self->StartWrite(&frame);
        if (!co_await std::move(result)) {
            self->stop();
        }
    }

  public:
    stream(server& owner, grpc::CallbackServerContext* context, uint64_t token, seastar::gate::holder lifetime)
        : _server(owner), _context(context), _lifetime(std::move(lifetime))
    {
        _route.token = token;
        if (auto auth = context->auth_context()) {
            for (const auto identity : auth->GetPeerIdentity()) {
                _peer_identities.emplace_back(identity.data(), identity.size());
            }
        }
    }
    void start() { StartRead(&_read); }
    void enqueue(gateway::MessageToGatewayFrame frame)
    {
        check_owner();
        if (_stopping) {
            throw io::stopped();
        }
        auto capacity = _server._writes.acquire(frame.ByteSizeLong());
        // 预约发生在投递处，Seastar 信号量维持帧顺序，无需额外 deque。
        auto slot = seastar::get_units(_write_slot, 1);
        auto self = shared_from_this();
        (void)seastar::with_scheduling_group(_server._io, [self, frame = std::move(frame),
                                                           capacity = std::move(capacity),
                                                           slot = std::move(slot)]() mutable {
            return write(std::move(self), std::move(frame), std::move(capacity), std::move(slot));
        }).handle_exception([self](std::exception_ptr error) {
            if (!self->_stopping) {
                LOG_WARN(logger, "[message/stream] write failed: {}", error);
                self->stop();
            }
        });
    }
    void stop()
    {
        check_owner();
        if (_stopping || _finished) {
            return;
        }
        _stopping = true;
        _write_slot.broken(io::stopped());
        _context->TryCancel();
        finish();
    }
    void OnCancel() override { stop(); }
    void OnReadDone(bool ok) override
    {
        check_owner();
        if (!ok || _stopping) {
            stop();
            return;
        }
        auto lifetime = _server._frames.hold();
        auto self = shared_from_this();
        (void)seastar::with_scheduling_group(_server._protocol,
                                             [self, frame = std::move(_read)]() mutable {
                                                 return self->_server.process(std::move(self), std::move(frame));
                                             })
            .handle_exception([self](std::exception_ptr error) {
                LOG_WARN(logger, "[message/stream] frame failed: {}", error);
                self->stop();
            })
            .finally([lifetime = std::move(lifetime)] {});
    }
    void OnWriteDone(bool ok) override
    {
        check_owner();
        assert(_write_done);
        auto* complete = std::exchange(_write_done, nullptr);
        if (!ok) {
            stop();
        }
        if (_stopping) {
            finish();
        }
        complete->set_value(ok);
    }
    void OnDone() override
    {
        check_owner();
        auto self = shared_from_this();
        _finished = true;
        _context = nullptr;
        _server.on_grpc_done(*this);
        // self 及发送任务继续持有对象和 _lifetime，直到全部帧释放后才归还 RPC gate。
    }
};
server::server(message_io::options config, seastar::scheduling_group protocol, seastar::scheduling_group io_group)
    : _options(std::move(config)), _protocol(protocol), _io(io_group),
      _writes(_options.max_writes, _options.max_write_bytes)
{
}
server::~server() { assert(_streams.empty() && _registered.empty()); }
void server::start(io::endpoint numeric_address, std::function<void(stream_route, gateway::CommandEnvelope)> command)
{
    _command = std::move(command);
    _server.start(std::move(numeric_address), _options.server_credentials, *this);
}
grpc::ServerBidiReactor<gateway::GatewayToMessageFrame, gateway::MessageToGatewayFrame>*
server::Connect(grpc::CallbackServerContext* context)
{
    auto connection = std::make_shared<stream>(*this, context, ++_next_token, _calls.hold());
    auto* result = connection.get();
    _streams.emplace(_next_token, std::move(connection));
    if (_stopping) {
        result->stop();
    } else {
        result->start();
    }
    return result;
}
void server::register_stream(stream& source, const gateway::RegisterGateway& registration)
{
    const bool valid =
        !_stopping && !source._registered && registration.protocol_version() == 1 &&
        !registration.gateway_id().empty() && !registration.instance_id().empty() &&
        message_route::owner_shard(registration.gateway_id(), _options.shards) == seastar::this_shard_id() &&
        (!_options.mtls ||
         std::ranges::find(source._peer_identities, registration.gateway_id()) != source._peer_identities.end());
    if (!valid) {
        source.stop();
        return;
    }
    source._route.gateway_id = registration.gateway_id();
    source._route.instance_id = registration.instance_id();
    source._route.shard_id = registration.shard_id();
    const stream_key key{source._route.gateway_id, source._route.instance_id, source._route.shard_id};
    if (const auto current = _registered.find(key); current != _registered.end()) {
        // 先替换索引，再取消旧流。旧 OnDone 只会移除自己的 token。
        const auto old = current->second;
        current->second = source._route.token;
        _streams.at(old)->stop();
    } else {
        _registered.emplace(key, source._route.token);
    }
    source._registered = true;
    gateway::MessageToGatewayFrame response;
    auto* result = response.mutable_register_result();
    result->set_accepted(true);
    result->set_message_node_id(_options.node_id);
    result->set_stream_epoch(registration.stream_epoch());
    source.enqueue(std::move(response));
    LOG_INFO(logger, "[message/stream] Gateway 流已注册 gateway={} gateway_shard={} message_shard={}",
             registration.gateway_id(), registration.shard_id(), seastar::this_shard_id());
}
seastar::future<> server::process(std::shared_ptr<stream> source, gateway::GatewayToMessageFrame frame)
{
    if (source->_stopping || _stopping) {
        source->stop();
        co_return;
    }
    if (frame.has_register_gateway()) {
        register_stream(*source, frame.register_gateway());
    } else if (!source->_registered) {
        source->stop();
    } else if (frame.has_heartbeat()) {
        gateway::MessageToGatewayFrame response;
        auto* heartbeat = response.mutable_heartbeat_ack();
        heartbeat->set_sequence(frame.heartbeat().sequence());
        heartbeat->set_sent_at_unix_ms(frame.heartbeat().sent_at_unix_ms());
        source->enqueue(std::move(response));
    } else if (frame.has_command()) {
        _command(source->_route, std::move(*frame.mutable_command()));
    } else if (!frame.has_client_forward_ack()) {
        source->stop();
    }
    if (!source->_stopping) {
        source->StartRead(&source->_read);
    }
}
void server::on_grpc_done(stream& source)
{
    if (source._registered) {
        const stream_key key{source._route.gateway_id, source._route.instance_id, source._route.shard_id};
        const auto current = _registered.find(key);
        if (current != _registered.end() && current->second == source._route.token) {
            _registered.erase(current);
        }
    }
    _streams.erase(source._route.token);
}
bool server::reply(const stream_route& route, gateway::MessageToGatewayFrame frame)
{
    if (_stopping) {
        return false;
    }
    const auto found = _registered.find({route.gateway_id, route.instance_id, route.shard_id});
    if (found == _registered.end() || found->second != route.token) {
        return false;
    }
    try {
        _streams.at(found->second)->enqueue(std::move(frame));
        return true;
    } catch (const io::overloaded&) {
        // 无法交付命令结果时关闭流，使 Gateway 的在途请求得到失败，而非永久等待。
        _streams.at(found->second)->stop();
        return false;
    } catch (const io::stopped&) {
        return false;
    }
}
gateway::ClientForwardStatus server::forward(gateway::SessionLease lease, gateway::ClientForwardEnvelope envelope)
{
    if (_stopping) {
        return gateway::CLIENT_FORWARD_STATUS_OFFLINE;
    }
    const auto found = _registered.find({lease.gateway_id(), lease.instance_id(), lease.shard_id()});
    if (found == _registered.end()) {
        return gateway::CLIENT_FORWARD_STATUS_OFFLINE;
    }
    gateway::MessageToGatewayFrame frame;
    *frame.mutable_client_forward() = std::move(envelope);
    try {
        _streams.at(found->second)->enqueue(std::move(frame));
        // 仅代表本 Message shard 接受发送；客户端送达状态仍由 ACK/数据库同步决定。
        return gateway::CLIENT_FORWARD_STATUS_QUEUED;
    } catch (const io::overloaded&) {
        return gateway::CLIENT_FORWARD_STATUS_BACKPRESSURED;
    } catch (const io::stopped&) {
        return gateway::CLIENT_FORWARD_STATUS_OFFLINE;
    }
}
void server::begin_stop()
{
    _stopping = true;
    for (const auto& [token, connection] : _streams) {
        connection->stop();
    }
}
seastar::future<> server::close()
{
    begin_stop();
    co_await _server.close([this]() -> seastar::future<> {
        // native server 已停止创建 RPC，才关闭回调 gate，覆盖停止期间到达的 Connect。
        co_await _frames.close();
        co_await _calls.close();
    });
}
} // namespace imm::message_transport
