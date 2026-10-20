#pragma once
#include "transaction/messages.hh"
#include "transaction/friends.hh"
#include "transaction/groups.hh"
#include "transaction/files.hh"

namespace imm::message_protocol
{
struct route;
}
namespace imm::message_transaction
{
struct request_permit {
    seastar::gate::holder lifetime;
    message_io::capacity_units capacity;
};
class service
{
    message_io::driver _driver;
    message_io::operation_queue _io;
    message_io::budget _requests_budget;
    seastar::gate _requests;
    seastar::scheduling_group _transaction;
    message_transport::server& _transport;
    std::function<seastar::future<gateway::ClientForwardStatus>(gateway::SessionLease, gateway::ClientForwardEnvelope)>
        _forward;
    messages _messages;
    friends _friends;
    groups _groups;
    files _files;
    bool _stopping{};
    seastar::future<> process(request_context context, request_permit permit, const message_protocol::route* route);
    seastar::future<> deliver(request_context& context);
    protocol::Packet stage_message(request_context& context, accepted_message accepted);
    void reply(const request_context& context, protocol::Packet response);

  public:
    service(message_io::options config, seastar::scheduling_group transaction, seastar::scheduling_group io_group,
            message_transport::server& transport);
    seastar::future<> start(std::function<seastar::future<gateway::ClientForwardStatus>(gateway::SessionLease,
                                                                                        gateway::ClientForwardEnvelope)>
                                forward);
    void submit(message_transport::stream_route origin, gateway::CommandEnvelope command);
    seastar::future<> stop();
    seastar::future<protocol::Packet> send_text(request_context& context);
    seastar::future<protocol::Packet> send_group_text(request_context& context);
    seastar::future<protocol::Packet> send_file(request_context& context);
    seastar::future<protocol::Packet> send_group_file(request_context& context);
    seastar::future<protocol::Packet> acknowledge(request_context& context);
    seastar::future<protocol::Packet> pull_messages(request_context& context);
    seastar::future<protocol::Packet> apply_friend(request_context& context);
    seastar::future<protocol::Packet> reply_friend(request_context& context);
    seastar::future<protocol::Packet> pull_friends(request_context& context);
    seastar::future<protocol::Packet> pull_friend_applications(request_context& context);
    seastar::future<protocol::Packet> create_group(request_context& context);
    seastar::future<protocol::Packet> apply_group(request_context& context);
    seastar::future<protocol::Packet> reply_group(request_context& context);
    seastar::future<protocol::Packet> pull_group(request_context& context);
    seastar::future<protocol::Packet> quit_group(request_context& context);
    seastar::future<protocol::Packet> kick_group_member(request_context& context);
    seastar::future<protocol::Packet> upload(request_context& context);
    seastar::future<protocol::Packet> download(request_context& context);
};
} // namespace imm::message_transaction
