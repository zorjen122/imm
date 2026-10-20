#pragma once
#include "transaction/context.hh"
#include "io/driver.hh"

namespace imm::message_transaction
{
class groups
{
    message_io::operation_queue& _io;
    void notify_application(request_context& context, int64_t master_uid, int64_t requestor_uid, int64_t group_id,
                            const std::string& request_message);
    void notify_change(request_context& context, const std::vector<int64_t>& recipients, int64_t group_id,
                       int64_t target_uid, int64_t actor_uid, protocol::GroupChangeKind change,
                       const std::string& group_name, bool accept = false);

  public:
    explicit groups(message_io::operation_queue& operations) : _io(operations) {}
    seastar::future<protocol::Packet> create(request_context& context);
    seastar::future<protocol::Packet> apply(request_context& context);
    seastar::future<protocol::Packet> pull(request_context& context);
    seastar::future<protocol::Packet> reply(request_context& context);
    seastar::future<protocol::Packet> quit(request_context& context);
    seastar::future<protocol::Packet> kick(request_context& context);
};
} // namespace imm::message_transaction
