#include "transaction/service.hh"
#include "api/protocol.hh"
#include "Logger.h"
#include <seastar/core/with_scheduling_group.hh>

namespace imm::message_transaction
{
service::service(message_io::options config, seastar::scheduling_group transaction, seastar::scheduling_group io_group,
                 message_transport::server& transport)
    : _driver(config), _io(_driver, config.queue, io_group),
      _requests_budget(config.max_requests, config.max_request_bytes), _transaction(transaction), _transport(transport),
      _messages(_io), _friends(_io), _groups(_io), _files(_io)
{
}
seastar::future<> service::start(
    std::function<seastar::future<gateway::ClientForwardStatus>(gateway::SessionLease, gateway::ClientForwardEnvelope)>
        forward)
{
    _forward = std::move(forward);
    co_await _driver.start();
    _io.start();
}
void service::reply(const request_context& context, protocol::Packet response)
{
    gateway::MessageToGatewayFrame frame;
    auto* result = frame.mutable_command_result();
    result->set_request_id(context.command.request_id());
    result->set_error(TcpPacketError(response));
    result->set_retryable(isRetryableError(result->error()));
    result->set_packet(SerializeTcpPacket(response));
    if (!_transport.reply(context.origin, std::move(frame))) {
        LOG_DEBUG(logger, "[message/reply] origin stream closed request={}", context.command.request_id());
    }
}
void service::submit(message_transport::stream_route origin, gateway::CommandEnvelope command)
{
    request_context context{std::move(origin), std::move(command), {}, {}};
    try {
        if (_stopping) {
            throw io::stopped();
        }
        request_permit permit{_requests.hold(),
                              _requests_budget.acquire(sizeof(request_context) + context.command.ByteSizeLong() +
                                                       context.command.packet().size())};
        const int error = message_protocol::decode(context);
        if (error != ErrorCodes::Success) {
            reply(context, MakeErrorPacket(error));
            return;
        }
        const auto* route = message_protocol::route_for(context.command.operation());
        (void)seastar::with_scheduling_group(_transaction, [this, context = std::move(context),
                                                            permit = std::move(permit), route]() mutable {
            return process(std::move(context), std::move(permit), route);
        }).handle_exception([](std::exception_ptr error) {
            LOG_ERROR(logger, "[message/transaction] unhandled completion: {}", error);
        });
    } catch (const io::overloaded&) {
        reply(context, MakeErrorPacket(ErrorCodes::ResourceExhausted));
    } catch (const io::stopped&) {
        reply(context, MakeErrorPacket(ErrorCodes::DependencyUnavailable));
    }
}
seastar::future<> service::process(request_context context, request_permit permit, const message_protocol::route* route)
{
    protocol::Packet response;
    try {
        message_io::get_session lookup{context.command.actor_uid(), context.command.actor_device_id()};
        const auto lease = co_await _io.submit(std::move(lookup));
        if (lease.gateway_id() != context.origin.gateway_id || lease.instance_id() != context.origin.instance_id ||
            lease.shard_id() != context.origin.shard_id || lease.connection_id() != context.command.connection_id() ||
            lease.generation() != context.command.connection_generation()) {
            response = MakeErrorPacket(ErrorCodes::AuthenticationRequired);
        } else {
            auto pending = (this->*route->handle)(context);
            response = co_await std::move(pending);
        }
    } catch (const io::overloaded&) {
        response = MakeErrorPacket(ErrorCodes::ResourceExhausted);
    } catch (const io::stopped&) {
        response = MakeErrorPacket(ErrorCodes::DependencyUnavailable);
    } catch (const std::exception& error) {
        LOG_WARN(logger, "[message/transaction] dependency failed request={} error={}", context.command.request_id(),
                 error.what());
        response = MakeErrorPacket(ErrorCodes::DependencyUnavailable);
    }
    const bool success = TcpPacketError(response) == ErrorCodes::Success;
    reply(context, std::move(response));
    if (success) {
        try {
            co_await deliver(context);
        } catch (const std::exception& error) {
            // 响应和持久化结果已经确定，推送失败由 ACK/补拉收束，不改写业务成功。
            LOG_WARN(logger, "[message/delivery] request={} error={}", context.command.request_id(), error.what());
        }
    }
}
seastar::future<> service::deliver(request_context& context)
{
    size_t index{};
    for (const delivery& delivery : context.deliveries) {
        message_io::get_sessions lookup{delivery.recipient_uid};
        auto leases = co_await _io.submit(lookup);
        auto envelope =
            message_protocol::make_forward(delivery, context.command.request_id() + ":" + std::to_string(index++));
        for (const gateway::SessionLease& lease : leases) {
            if (lease.device_id() == delivery.excluded_device_id) {
                continue;
            }
            message_io::get_session current_lookup{delivery.recipient_uid, lease.device_id()};
            const auto current = co_await _io.submit(std::move(current_lookup));
            if (current.SerializeAsString() != lease.SerializeAsString()) {
                continue;
            }
            auto target = envelope;
            target.set_forward_id(envelope.forward_id() + ":" + lease.device_id());
            target.set_recipient_device_id(lease.device_id());
            target.set_expected_connection_id(lease.connection_id());
            target.set_expected_connection_generation(lease.generation());
            co_await _forward(lease, std::move(target));
        }
    }
}
protocol::Packet service::stage_message(request_context& context, accepted_message accepted)
{
    if (accepted.should_forward) {
        for (const int64_t recipient : accepted.recipients) {
            context.deliveries.push_back({recipient, accepted.forward_packet, {}});
        }
        context.deliveries.push_back(
            {context.command.actor_uid(), std::move(accepted.forward_packet), context.command.actor_device_id()});
    }
    return std::move(accepted.response);
}
seastar::future<protocol::Packet> service::send_text(request_context& context)
{
    co_return stage_message(context, co_await _messages.accept_text(context.command.actor_uid(), context.packet));
}
seastar::future<protocol::Packet> service::send_group_text(request_context& context)
{
    co_return stage_message(context, co_await _messages.accept_group_text(context.command.actor_uid(), context.packet));
}
seastar::future<protocol::Packet> service::send_file(request_context& context)
{
    co_return stage_message(context, co_await _messages.accept_file(context.command.actor_uid(), context.packet));
}
seastar::future<protocol::Packet> service::send_group_file(request_context& context)
{
    co_return stage_message(context, co_await _messages.accept_group_file(context.command.actor_uid(), context.packet));
}
seastar::future<protocol::Packet> service::acknowledge(request_context& context)
{
    auto result =
        co_await _messages.acknowledge(context.command.actor_uid(), context.packet, context.command.actor_device_id());
    if (result.should_forward_read) {
        context.deliveries.push_back({result.sender_uid, std::move(result.read_receipt), {}});
    }
    co_return std::move(result.response);
}
seastar::future<protocol::Packet> service::pull_messages(request_context& context)
{
    return _messages.pull_conversation(context.command.actor_uid(), context.packet);
}
seastar::future<protocol::Packet> service::apply_friend(request_context& context) { return _friends.apply(context); }
seastar::future<protocol::Packet> service::reply_friend(request_context& context) { return _friends.reply(context); }
seastar::future<protocol::Packet> service::pull_friends(request_context& context)
{
    return _friends.pull_friends(context);
}
seastar::future<protocol::Packet> service::pull_friend_applications(request_context& context)
{
    return _friends.pull_applications(context);
}
seastar::future<protocol::Packet> service::create_group(request_context& context) { return _groups.create(context); }
seastar::future<protocol::Packet> service::apply_group(request_context& context) { return _groups.apply(context); }
seastar::future<protocol::Packet> service::reply_group(request_context& context) { return _groups.reply(context); }
seastar::future<protocol::Packet> service::pull_group(request_context& context) { return _groups.pull(context); }
seastar::future<protocol::Packet> service::quit_group(request_context& context) { return _groups.quit(context); }
seastar::future<protocol::Packet> service::kick_group_member(request_context& context) { return _groups.kick(context); }
seastar::future<protocol::Packet> service::upload(request_context& context) { return _files.upload(context); }
seastar::future<protocol::Packet> service::download(request_context& context) { return _files.download(context); }
seastar::future<> service::stop()
{
    _stopping = true;
    _io.begin_stop();
    _driver.abort();
    co_await _io.stop();
    co_await _requests.close();
    co_await _driver.close();
}
} // namespace imm::message_transaction
