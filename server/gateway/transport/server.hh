#pragma once
#include "transaction/service.hh"
#include <seastar/core/sharded.hh>
#include <seastar/net/api.hh>
#include <unordered_map>
#include <optional>

namespace wimi::gateway_transport
{
class connection;
struct scheduling_groups {
    seastar::scheduling_group protocol;
    seastar::scheduling_group transaction;
    seastar::scheduling_group io;
};
class server
{
    gateway_io::options _options;
    scheduling_groups _groups;
    seastar::sharded<server>* _shards;
    gateway_transaction::service _transaction;
    std::optional<seastar::server_socket> _listener;
    std::optional<seastar::future<>> _accept;
    seastar::gate _connections_gate;
    std::unordered_map<std::string, std::shared_ptr<connection>> _connections;
    bool _stopping{};
    bool _stopped{};
    seastar::future<> accept_loop();
    static seastar::future<> run_connection(server& owner, std::shared_ptr<connection> session);

  public:
    server(gateway_io::options config, scheduling_groups groups, seastar::sharded<server>* shards);
    seastar::future<> start();
    seastar::future<> listen();
    seastar::future<> stop();
    gateway::ClientForwardStatus deliver(const gateway::ClientForwardEnvelope& forward);
    seastar::future<> close_previous(gateway::SessionLease previous);
    void close_connection(const std::string& id);
    gateway_transaction::service& transaction() { return _transaction; }
    const gateway_io::options& config() const { return _options; }
    const scheduling_groups& groups() const { return _groups; }
};
} // namespace wimi::gateway_transport
