#pragma once
#include "transaction/context.hh"
#include "io/driver.hh"

namespace wimi::message_transaction
{
class files
{
    message_io::operation_queue& _io;

  public:
    explicit files(message_io::operation_queue& operations) : _io(operations) {}
    seastar::future<protocol::Packet> upload(request_context& context);
    seastar::future<protocol::Packet> download(request_context& context);
};
} // namespace wimi::message_transaction
