#include "service.hh"
#include "message_route.hh"
#include "Logger.h"
#include <fmt/format.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace imm::message
{
shard_service::shard_service(message_io::options config, scheduling_groups groups,
                             seastar::sharded<shard_service>* services)
    : _options(config), _services(*services), _transport(config, groups.protocol, groups.io),
      _transaction(std::move(config), groups.transaction, groups.io, _transport)
{
}
seastar::future<> shard_service::start()
{
    if (_options.shards != seastar::this_smp_shard_count()) {
        throw std::invalid_argument("Message SMP count must match self.shards and State topology");
    }
    co_await _transaction.start([this](gateway::SessionLease lease, gateway::ClientForwardEnvelope envelope) {
        return forward(std::move(lease), std::move(envelope));
    });
    const auto ip = co_await _dns.resolve_name(_options.listener.host);
    const auto port = static_cast<uint16_t>(_options.listener.port + seastar::this_shard_id());
    _transport.start({fmt::format("{}", ip), port},
                     [this](message_transport::stream_route origin, gateway::CommandEnvelope command) {
                         _transaction.submit(std::move(origin), std::move(command));
                     });
    LOG_INFO(logger, "[message/io] ready shard={} tid={} port={}", seastar::this_shard_id(), syscall(SYS_gettid), port);
}
seastar::future<gateway::ClientForwardStatus> shard_service::forward(gateway::SessionLease lease,
                                                                     gateway::ClientForwardEnvelope envelope)
{
    const auto owner = message_route::owner_shard(lease.gateway_id(), _options.shards);
    return _services.invoke_on(
        owner, [lease = std::move(lease), envelope = std::move(envelope)](shard_service& target) mutable {
            return target._transport.forward(std::move(lease), std::move(envelope));
        });
}
seastar::future<> shard_service::stop()
{
    // 主流程在 io 组显式等待停止后，sharded::stop() 销毁实例前还会调用 stop()。
    if (std::exchange(_stopped, true)) {
        co_return;
    }
    _transport.begin_stop();
    co_await _transaction.stop();
    co_await _transport.close();
    co_await _dns.close();
    co_await io::drain_local_grpc();
}
} // namespace imm::message
