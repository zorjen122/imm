#pragma once
#include "io/driver.hh"
#include "IoQueue.h"
#include <seastar/core/gate.hh>
#include <seastar/core/scheduling.hh>

namespace imm::gateway_transport
{
class connection;
}
namespace imm::gateway_transaction
{
struct request_permit {
    seastar::gate::holder lifetime;
    gateway_io::capacity_units capacity;
};

// imm::gateway_transaction::service 是业务处理的核心类，它通过 driver 的消费模型，
// 可以使它只关注业务处理所需要的要素，而无需关注 rpc/redis/mysql 等 io 操作的实现细节。
class service
{
    gateway_io::options _options;
    gateway_io::driver _driver;
    io::operation_queue<gateway_io::driver, gateway_io::io_operation> _io;
    gateway_io::budget _requests_budget;
    seastar::gate _requests;
    seastar::scheduling_group _transaction;
    bool _stopping{};

  public:
    service(gateway_io::options config, seastar::scheduling_group transaction, seastar::scheduling_group io_group);
    gateway_io::driver& driver() { return _driver; }
    request_permit admit(size_t bytes);
    seastar::future<> start(std::function<gateway::ClientForwardStatus(const gateway::ClientForwardEnvelope&)> forward);
    seastar::future<> wait_ready();
    seastar::future<> login(std::shared_ptr<gateway_transport::connection> connection, protocol::ClientRequest request);
    seastar::future<> forward(std::shared_ptr<gateway_transport::connection> connection,
                              protocol::ClientRequest request);
    seastar::future<> remove(int64_t uid, gateway::SessionLease lease);
    void abort();
    void cancel(const std::string& connection_id);
    seastar::future<bool> refresh(int64_t uid, gateway::SessionLease lease);
    seastar::future<> stop();
};
} // namespace imm::gateway_transaction
