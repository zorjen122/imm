#include "transport/server.hh"
#include "transport/connection.hh"
#include "Logger.h"
#include <seastar/core/coroutine.hh>
#include <seastar/core/with_scheduling_group.hh>

namespace imm::gateway_transport
{
server::server(gateway_io::options config, scheduling_groups groups, seastar::sharded<server>* shards)
    : _options(std::move(config)), _groups(groups), _shards(shards),
      _transaction(_options, groups.transaction, groups.io)
{
}
seastar::future<> server::start()
{
    co_await _transaction.start([this](const gateway::ClientForwardEnvelope& forward) { return deliver(forward); });
    co_await _transaction.wait_ready();
}
seastar::future<> server::listen()
{
    seastar::listen_options options;
    options.reuse_address = true;
    options.listen_backlog = 1024;
    _listener.emplace(seastar::listen(seastar::socket_address(seastar::ipv4_addr(_options.port)), options));
    _accept.emplace(accept_loop());
    co_return;
}
seastar::future<> server::run_connection(server& owner, std::shared_ptr<connection> session)
{
    try {
        co_await session->run();
    } catch (const std::exception& error) {
        LOG_WARN(logger, "[gateway/connection] {}", error.what());
        session->close();
    }
    owner._connections.erase(session->id());
}
seastar::future<> server::accept_loop()
{
    while (!_stopping) {
        try {
            auto accepted = co_await _listener->accept();
            auto session = std::make_shared<connection>(std::move(accepted.connection), *this);
            _connections.emplace(session->id(), session);
            (void)seastar::with_gate(_connections_gate, [this, session] { return run_connection(*this, session); });
        } catch (const std::exception& error) {
            if (!_stopping) {
                LOG_WARN(logger, "[gateway/accept] {}", error.what());
            }
        }
    }
}
void server::close_connection(const std::string& id)
{
    if (const auto found = _connections.find(id); found != _connections.end()) {
        found->second->close();
    }
}
seastar::future<> server::close_previous(gateway::SessionLease previous)
{
    if (previous.gateway_id() != _options.gateway_id || previous.instance_id() != _options.instance_id) {
        co_return;
    }
    if (previous.shard_id() >= seastar::this_smp_shard_count()) {
        co_return;
    }
    // 只投递连接身份，目标 shard 自己操作 socket；即使旧登录尚未完成发布，
    // 它的 connection_id 从 accept 起就存在，关闭标志可阻止旧协程恢复后继续绑定。
    co_await _shards->invoke_on(previous.shard_id(),
                                [id = previous.connection_id()](server& local) { local.close_connection(id); });
}
gateway::ClientForwardStatus server::deliver(const gateway::ClientForwardEnvelope& forward)
{
    auto found = _connections.find(forward.expected_connection_id());
    if (found == _connections.end()) {
        return gateway::CLIENT_FORWARD_STATUS_OFFLINE;
    }
    auto& session = *found->second;
    if (session.closed() || session.uid() != forward.recipient_uid() ||
        session.lease().device_id() != forward.recipient_device_id() ||
        session.lease().generation() != forward.expected_connection_generation()) {
        return gateway::CLIENT_FORWARD_STATUS_STALE_ROUTE;
    }
    TcpPacket packet;
    if (!packet.ParseFromString(forward.packet())) {
        return gateway::CLIENT_FORWARD_STATUS_BACKPRESSURED;
    }
    const auto sequence = TcpPacketReliableSequence(packet);
    ClientEnvelope envelope;
    envelope.mutable_push()->set_delivery_id(forward.message_id() > 0 ? forward.message_id() : sequence);
    envelope.mutable_push()->set_type(TcpPacketPushType(packet));
    *envelope.mutable_push()->mutable_packet() = std::move(packet);
    const auto wire = envelope.SerializeAsString();
    const bool accepted = sequence > 0 ? session.send_reliable(wire, sequence) : session.send(wire);
    return accepted ? gateway::CLIENT_FORWARD_STATUS_QUEUED : gateway::CLIENT_FORWARD_STATUS_BACKPRESSURED;
}
seastar::future<> server::stop()
{
    if (_stopped) {
        co_return;
    }
    _stopping = true;
    if (_listener) {
        _listener->abort_accept();
    }
    for (auto& [id, session] : _connections) {
        session->close();
    }
    _transaction.abort();
    if (_accept) {
        co_await std::move(*_accept);
        _accept.reset();
    }
    co_await _connections_gate.close();
    co_await _transaction.stop();
    _stopped = true;
}
} // namespace imm::gateway_transport
