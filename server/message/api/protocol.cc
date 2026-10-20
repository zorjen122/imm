#include "api/protocol.hh"
#include "transaction/service.hh"
#include <array>

namespace wimi::message_protocol
{
namespace
{
using transaction = message_transaction::service;
const std::array registry{
    route{protocol::OP_SEND_TEXT, &transaction::send_text},
    route{protocol::OP_SEND_GROUP_TEXT, &transaction::send_group_text},
    route{protocol::OP_SEND_FILE, &transaction::send_file},
    route{protocol::OP_SEND_GROUP_FILE, &transaction::send_group_file},
    route{protocol::OP_ACKNOWLEDGE, &transaction::acknowledge},
    route{protocol::OP_PULL_CONVERSATION_MESSAGES, &transaction::pull_messages},
    route{protocol::OP_ADD_FRIEND, &transaction::apply_friend},
    route{protocol::OP_REPLY_FRIEND, &transaction::reply_friend},
    route{protocol::OP_PULL_FRIEND_LIST, &transaction::pull_friends},
    route{protocol::OP_PULL_FRIEND_APPLICATIONS, &transaction::pull_friend_applications},
    route{protocol::OP_CREATE_GROUP, &transaction::create_group},
    route{protocol::OP_JOIN_GROUP, &transaction::apply_group},
    route{protocol::OP_REPLY_JOIN_GROUP, &transaction::reply_group},
    route{protocol::OP_PULL_GROUP_MEMBERS, &transaction::pull_group},
    route{protocol::OP_QUIT_GROUP, &transaction::quit_group},
    route{protocol::OP_KICK_GROUP_MEMBER, &transaction::kick_group_member},
    route{protocol::OP_UPLOAD_FILE, &transaction::upload},
    route{protocol::OP_UPLOAD_FILE_CHUNK, &transaction::upload},
    route{protocol::OP_COMPLETE_FILE_UPLOAD, &transaction::upload},
    route{protocol::OP_DOWNLOAD_FILE, &transaction::download},
};
} // namespace
std::span<const route> routes() { return registry; }
const route* route_for(protocol::Operation operation)
{
    for (const route& endpoint : registry) {
        if (endpoint.operation == operation) {
            return &endpoint;
        }
    }
    return nullptr;
}
int decode(message_transaction::request_context& context)
{
    const auto& command = context.command;
    if (command.actor_uid() <= 0 || command.request_id().empty() || command.connection_id().empty() ||
        !command.connection_generation() || command.actor_device_id().empty() ||
        command.actor_device_id().size() > 36) {
        return ErrorCodes::AuthenticationRequired;
    }
    if (!context.packet.ParseFromString(command.packet()) ||
        !TcpRequestMatchesOperation(command.operation(), context.packet)) {
        return ErrorCodes::ProtobufPacket;
    }
    if (!route_for(command.operation())) {
        return ErrorCodes::NotFound;
    }
    return ErrorCodes::Success;
}
gateway::ClientForwardEnvelope make_forward(const message_transaction::delivery& delivery, std::string forward_id)
{
    gateway::ClientForwardEnvelope result;
    result.set_forward_id(std::move(forward_id));
    result.set_recipient_uid(delivery.recipient_uid);
    result.set_packet(SerializeTcpPacket(delivery.packet));
    result.set_message_id(TcpPacketReliableSequence(delivery.packet));
    if (delivery.packet.has_text_message_push()) {
        const auto& push = delivery.packet.text_message_push();
        result.set_conversation_id(push.conversation_id());
        result.set_conversation_seq(push.conversation_seq());
    } else if (delivery.packet.has_file_message_push()) {
        const auto& push = delivery.packet.file_message_push();
        result.set_conversation_id(push.conversation_id());
        result.set_conversation_seq(push.conversation_seq());
    } else if (delivery.packet.has_read_receipt_push()) {
        const auto& push = delivery.packet.read_receipt_push();
        result.set_conversation_id(push.conversation_id());
        result.set_conversation_seq(push.conversation_seq());
    }
    return result;
}
} // namespace wimi::message_protocol
