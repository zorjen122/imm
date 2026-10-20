#pragma once
#include "transaction/context.hh"
#include <seastar/core/future.hh>
#include <span>

namespace imm::message_transaction
{
class service;
}
namespace imm::message_protocol
{
struct route {
    protocol::Operation operation;
    seastar::future<protocol::Packet> (message_transaction::service::*handle)(message_transaction::request_context&);
};
std::span<const route> routes();
const route* route_for(protocol::Operation operation);
// 只检查信封、payload 对应关系与身份字段边界；当前 lease 的一致性由异步事务校验。
int decode(message_transaction::request_context& context);
gateway::ClientForwardEnvelope make_forward(const message_transaction::delivery& delivery, std::string forward_id);
} // namespace imm::message_protocol
