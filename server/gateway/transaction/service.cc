#include "transaction/service.hh"
#include "transport/connection.hh"
#include "Logger.h"
#include <seastar/core/coroutine.hh>
#include <cassert>

namespace wimi::gateway_transaction
{
using namespace gateway_io;
service::service(options config, seastar::scheduling_group transaction, seastar::scheduling_group io_group)
    : _options(std::move(config)), _driver(_options), _io(_driver, _options.queue, io_group),
      _requests_budget(_options.max_requests, _options.max_request_bytes), _transaction(transaction)
{
}
request_permit service::admit(size_t bytes)
{
    if (_stopping) {
        throw io::stopped();
    }
    auto units = _requests_budget.acquire(bytes);
    return {_requests.hold(), std::move(units)};
}
seastar::future<>
service::start(std::function<gateway::ClientForwardStatus(const gateway::ClientForwardEnvelope&)> forward)
{
    co_await _driver.start(std::move(forward));
    _io.start();
}
seastar::future<> service::wait_ready() { return _driver.wait_ready(); }
seastar::future<> service::login(std::shared_ptr<gateway_transport::connection> connection,
                                 protocol::ClientRequest request)
{
    assert(seastar::current_scheduling_group() == _transaction);
    const auto& login = request.packet().login_request();
    if (connection->uid() > 0) {
        if (connection->uid() != login.uid() || connection->lease().device_id() != login.device_id()) {
            connection->error(request, ErrorCodes::UidInvalid);
            co_return;
        }
    }
    if (!co_await _io.submit(validate_token{login.uid(), login.auth_token()})) {
        connection->error(request, ErrorCodes::TokenInvalid);
        co_return;
    }
    if (connection->closed()) {
        co_return;
    }
    if (connection->uid() == 0) {
        if (!co_await _io.submit(prepare_profile{login})) {
            connection->error(request, ErrorCodes::NotFound);
            co_return;
        }
        if (connection->closed()) {
            co_return;
        }
        if (!co_await _io.submit(register_device{login.uid(), login.device_id()})) {
            connection->error(request, ErrorCodes::AuthenticationRequired);
            co_return;
        }
        if (connection->closed()) {
            co_return;
        }
        gateway::SessionLease route;
        route.set_device_id(login.device_id());
        route.set_gateway_id(_options.gateway_id);
        route.set_instance_id(_options.instance_id);
        route.set_shard_id(seastar::this_shard_id());
        route.set_connection_id(connection->id());
        auto bound = co_await _io.submit(bind_session{login.uid(), std::move(route)});
        // 先保存本次身份，使断连清理也能按 generation 条件删除自己的租约。
        // 旧连接从 accept 起就已登记；即使其登录协程仍挂起，也能先标记关闭。
        connection->bind(login.uid(), std::move(bound.current));
        if (bound.previous) {
            co_await connection->close_previous(std::move(*bound.previous));
        }
        if (connection->closed()) {
            co_return;
        }
        if (!co_await _io.submit(refresh_session{connection->uid(), connection->lease()})) {
            connection->close();
            co_return;
        }
    }
    if (connection->closed()) {
        co_return;
    }
    protocol::Packet response;
    response.mutable_login_response()->set_uid(login.uid());
    SetTcpPacketStatus(response, ErrorCodes::Success);
    connection->respond(request, std::move(response));
}
seastar::future<> service::forward(std::shared_ptr<gateway_transport::connection> connection,
                                   protocol::ClientRequest request)
{
    assert(seastar::current_scheduling_group() == _transaction);
    if (connection->closed() || !co_await connection->refresh()) {
        co_return;
    }
    gateway::CommandEnvelope command;
    command.set_request_id(request.request_id());
    command.set_actor_uid(connection->uid());
    command.set_actor_device_id(connection->lease().device_id());
    command.set_connection_id(connection->id());
    command.set_connection_generation(connection->lease().generation());
    command.set_operation(request.operation());
    command.set_conversation_id(TcpPacketConversationId(request.packet()));
    command.set_packet(request.packet().SerializeAsString());
    auto ticket = co_await _io.submit(send_command{std::move(command)});
    if (connection->closed()) {
        _driver.cancel(connection->id());
    }
    auto response = co_await std::move(ticket.result);
    if (connection->closed()) {
        co_return;
    }
    if (response.error() == ErrorCodes::AuthenticationRequired) {
        connection->close();
        co_return;
    }
    if (request.operation() == protocol::OP_ACKNOWLEDGE) {
        co_return;
    }
    protocol::Packet packet;
    if (!packet.ParseFromString(response.packet())) {
        connection->error(request, ErrorCodes::ProtobufPacket);
        co_return;
    }
    connection->respond(request, std::move(packet));
}
seastar::future<bool> service::refresh(int64_t uid, gateway::SessionLease lease)
{
    return _io.submit(refresh_session{uid, std::move(lease)});
}
seastar::future<> service::remove(int64_t uid, gateway::SessionLease lease)
{
    co_await _io.submit(clear_session{uid, std::move(lease)});
}
void service::cancel(const std::string& connection_id) { _driver.cancel(connection_id); }
void service::abort()
{
    if (_stopping) {
        return;
    }
    _stopping = true;
    _io.begin_stop();
    _driver.abort();
}
seastar::future<> service::stop()
{
    abort();
    co_await _io.stop();
    co_await _requests.close();
    co_await _driver.stop();
}
} // namespace wimi::gateway_transaction
