#include "service.hh"
#include "Logger.h"
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <fmt/format.h>
#include <cassert>
#include <utility>

namespace imm::state_service
{
class unary_call final : public grpc::ServerUnaryReactor
{
    seastar::gate::holder _lifetime;
    seastar::semaphore_units<> _capacity;
    const unsigned _owner{seastar::this_shard_id()};

  public:
    unary_call(seastar::gate::holder lifetime, seastar::semaphore_units<> capacity)
        : _lifetime(std::move(lifetime)), _capacity(std::move(capacity))
    {
    }
    void OnDone() override
    {
        assert(seastar::engine_is_ready() && seastar::this_shard_id() == _owner);
        delete this;
    }
};
service::service(options config, scheduling_groups groups)
    : _options(std::move(config)), _groups(groups), _capacity(_options.max_requests)
{
}
seastar::future<> service::start()
{
    const auto ip = co_await _dns.resolve_name(_options.listener.host);
    _server.start({fmt::format("{}", ip), _options.listener.port}, grpc::InsecureServerCredentials(), *this, true);
    LOG_INFO(logger, "[state/io] ready shard={} port={}", seastar::this_shard_id(), _server.port());
}
seastar::future<> service::execute(unary_call* call, std::function<grpc::Status()> operation,
                                   seastar::gate::holder lifetime)
{
    grpc::Status status;
    try {
        const bool accepting = co_await seastar::with_scheduling_group(
            _groups.protocol, [this] { return seastar::make_ready_future<bool>(!_stopping); });
        if (accepting) {
            status = co_await seastar::with_scheduling_group(_groups.transaction, [operation = std::move(operation)] {
                return seastar::make_ready_future<grpc::Status>(operation());
            });
        } else {
            status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "State is stopping");
        }
    } catch (const std::exception& error) {
        status = grpc::Status(grpc::StatusCode::INTERNAL, error.what());
    }
    // 请求及 response 由 gRPC 持有至 OnDone；Finish 之后任务不再访问 call/response。
    // lifetime 单独保护调度任务，避免 OnDone 先执行时服务已被释放。
    co_await seastar::with_scheduling_group(_groups.io, [call, status = std::move(status)] { call->Finish(status); });
}
grpc::ServerUnaryReactor* service::submit(grpc::CallbackServerContext& context, std::function<grpc::Status()> operation)
{
    if (_stopping) {
        auto* rejected = context.DefaultReactor();
        rejected->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE, "State is stopping"));
        return rejected;
    }
    auto capacity = seastar::try_get_units(_capacity, 1);
    if (!capacity) {
        auto* rejected = context.DefaultReactor();
        rejected->Finish(grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "State request capacity exceeded"));
        return rejected;
    }
    // 在跨 scheduling group 投递前预约，直到 OnDone 才归还，包含响应发送中的请求。
    auto* call = new unary_call(_calls.hold(), std::move(*capacity));
    (void)execute(call, std::move(operation), _tasks.hold());
    return call;
}
grpc::ServerUnaryReactor* service::PickConnectionGateway(grpc::CallbackServerContext* context,
                                                         const ::state::ConnectUser*, ::state::ConnectUserRsp* response)
{
    return submit(*context, [this, response] {
        const auto count = _options.gateways.size();
        if (count == 0) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no connection gateway configured");
        }
        const auto start = _next_gateway++ % count;
        for (std::size_t offset = 0; offset < count; ++offset) {
            const auto& node = _options.gateways[(start + offset) % count];
            if (!node.active) {
                continue;
            }
            response->set_ip(node.host);
            response->set_port(node.port);
            response->set_node_id(node.id);
            return grpc::Status::OK;
        }
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no active connection gateway");
    });
}
grpc::ServerUnaryReactor* service::ListMessageNodes(grpc::CallbackServerContext* context,
                                                    const ::state::TopologyRequest*, ::state::MessageTopology* response)
{
    return submit(*context, [this, response] {
        *response = _options.messages;
        return grpc::Status::OK;
    });
}
seastar::future<> service::stop()
{
    if (std::exchange(_stopped, true)) {
        co_return;
    }
    _stopping = true;
    co_await _server.close([this]() -> seastar::future<> {
        co_await _tasks.close();
        co_await _calls.close();
    });
    co_await _dns.close();
    co_await io::drain_local_grpc();
}
} // namespace imm::state_service
