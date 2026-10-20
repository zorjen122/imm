#pragma once
#include "transaction/context.hh"
#include "io/driver.hh"

namespace imm::message_transaction
{
class friends
{
    message_io::operation_queue& _io;
    seastar::future<int> store_application(int64_t actor_uid, const protocol::AddFriendRequest& request);
    seastar::future<int> store_reply(int64_t actor_uid, const protocol::ReplyFriendRequest& request);

  public:
    explicit friends(message_io::operation_queue& operations) : _io(operations) {}
    seastar::future<protocol::Packet> apply(request_context& context);
    seastar::future<protocol::Packet> reply(request_context& context);
    seastar::future<protocol::Packet> pull_applications(request_context& context);
    seastar::future<protocol::Packet> pull_friends(request_context& context);
};
} // namespace imm::message_transaction
