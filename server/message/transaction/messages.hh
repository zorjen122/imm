#pragma once
#include "TcpMessageCodec.h"
#include "io/driver.hh"

namespace imm::message_transaction
{
struct accepted_message {
    protocol::Packet response;
    protocol::Packet forward_packet;
    std::vector<int64_t> recipients;
    bool should_forward{};
};
struct acknowledged_message {
    protocol::Packet response;
    protocol::Packet read_receipt;
    int64_t sender_uid{};
    bool should_forward_read{};
};
class messages
{
    message_io::operation_queue& _io;

  public:
    explicit messages(message_io::operation_queue& operations) : _io(operations) {}
    // 调用者持有 request 直至 future 完成；提交的 I/O 参数均复制进有类型的操作对象。
    seastar::future<accepted_message> accept_text(int64_t actor_uid, const protocol::Packet& request);
    seastar::future<accepted_message> accept_group_text(int64_t actor_uid, const protocol::Packet& request);
    seastar::future<accepted_message> accept_file(int64_t actor_uid, const protocol::Packet& request);
    seastar::future<accepted_message> accept_group_file(int64_t actor_uid, const protocol::Packet& request);
    seastar::future<acknowledged_message> acknowledge(int64_t actor_uid, const protocol::Packet& request,
                                                      const std::string& device_id);
    seastar::future<protocol::Packet> pull_conversation(int64_t actor_uid, const protocol::Packet& request);
};
} // namespace imm::message_transaction
