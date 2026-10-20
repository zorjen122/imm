#pragma once
#include "transaction/service.hh"
#include <seastar/core/sharded.hh>

namespace wimi::message
{
struct scheduling_groups {
    seastar::scheduling_group protocol;
    seastar::scheduling_group transaction;
    seastar::scheduling_group io;
};
// 应用资源的 shard 所有者；跨 shard 只传送 lease 和 protobuf，不传递 stream/连接指针。
class shard_service
{
    message_io::options _options;
    seastar::sharded<shard_service>& _services;
    io::dns_resolver _dns;
    message_transport::server _transport;
    message_transaction::service _transaction;
    bool _stopped{};
    seastar::future<gateway::ClientForwardStatus> forward(gateway::SessionLease lease,
                                                          gateway::ClientForwardEnvelope envelope);

  public:
    shard_service(message_io::options config, scheduling_groups groups, seastar::sharded<shard_service>* services);
    seastar::future<> start();
    seastar::future<> stop();
};
} // namespace wimi::message
