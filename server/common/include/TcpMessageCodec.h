#pragma once

#include <cstdint>
#include <string>

#include "Const.h"
#include "tcp_message.pb.h"

namespace wimi
{

using TcpPacket = protocol::Packet;
using ClientEnvelope = protocol::ClientEnvelope;

inline bool ParseTcpPacket(const std::string& data, TcpPacket& packet) { return packet.ParseFromString(data); }

inline bool ParseClientEnvelope(const std::string& data, ClientEnvelope& envelope)
{
    return envelope.ParseFromString(data);
}

inline std::string SerializeTcpPacket(const TcpPacket& packet)
{
    std::string data;
    packet.SerializeToString(&data);
    return data;
}

inline std::string SerializeClientEnvelope(const ClientEnvelope& envelope)
{
    std::string data;
    envelope.SerializeToString(&data);
    return data;
}

inline void SetTcpPacketStatus(TcpPacket& packet, int error, const std::string& message = {})
{
    auto* status = packet.mutable_status();
    status->set_error(error);
    if (!message.empty()) {
        status->set_message(message);
    }
}

inline TcpPacket MakeErrorPacket(int error, const std::string& message = {})
{
    TcpPacket packet;
    SetTcpPacketStatus(packet, error, message);
    return packet;
}

inline int TcpPacketError(const TcpPacket& packet)
{
    return packet.has_status() ? packet.status().error() : ErrorCodes::Success;
}

inline protocol::Operation TcpPacketRequestOperation(const TcpPacket& packet)
{
    if (packet.has_pull_friend_list_request())
        return protocol::Operation::OP_PULL_FRIEND_LIST;
    if (packet.has_pull_friend_apply_list_request())
        return protocol::Operation::OP_PULL_FRIEND_APPLICATIONS;
    if (packet.has_pull_conversation_messages_request())
        return protocol::Operation::OP_PULL_CONVERSATION_MESSAGES;
    if (packet.has_pull_group_members_request())
        return protocol::Operation::OP_PULL_GROUP_MEMBERS;
    if (packet.has_ping_request())
        return protocol::Operation::OP_PING;
    if (packet.has_login_request())
        return protocol::Operation::OP_LOGIN;
    if (packet.has_quit_request())
        return protocol::Operation::OP_QUIT;
    if (packet.has_update_user_info_request())
        return protocol::Operation::OP_UPDATE_USER_INFO;
    if (packet.has_search_user_request())
        return protocol::Operation::OP_SEARCH_USER;
    if (packet.has_add_friend_request())
        return protocol::Operation::OP_ADD_FRIEND;
    if (packet.has_reply_friend_request())
        return protocol::Operation::OP_REPLY_FRIEND;
    if (packet.has_remove_friend_request())
        return protocol::Operation::OP_REMOVE_FRIEND;
    if (packet.has_send_text_request())
        return protocol::Operation::OP_SEND_TEXT;
    if (packet.has_send_file_request())
        return protocol::Operation::OP_SEND_FILE;
    if (packet.has_file_upload_request())
        return protocol::Operation::OP_UPLOAD_FILE;
    if (packet.has_receipt())
        return protocol::Operation::OP_ACKNOWLEDGE;
    if (packet.has_create_group_request())
        return protocol::Operation::OP_CREATE_GROUP;
    if (packet.has_join_group_request())
        return protocol::Operation::OP_JOIN_GROUP;
    if (packet.has_reply_join_group_request())
        return protocol::Operation::OP_REPLY_JOIN_GROUP;
    if (packet.has_quit_group_request())
        return protocol::Operation::OP_QUIT_GROUP;
    if (packet.has_send_group_text_request())
        return protocol::Operation::OP_SEND_GROUP_TEXT;
    if (packet.has_file_upload_chunk_request())
        return protocol::Operation::OP_UPLOAD_FILE_CHUNK;
    if (packet.has_file_upload_complete_request())
        return protocol::Operation::OP_COMPLETE_FILE_UPLOAD;
    if (packet.has_file_download_request())
        return protocol::Operation::OP_DOWNLOAD_FILE;
    if (packet.has_kick_group_member_request())
        return protocol::Operation::OP_KICK_GROUP_MEMBER;
    if (packet.has_send_group_file_request())
        return protocol::Operation::OP_SEND_GROUP_FILE;
    return protocol::Operation::OP_UNSPECIFIED;
}

inline bool TcpRequestMatchesOperation(protocol::Operation operation, const TcpPacket& packet)
{
    return operation != protocol::Operation::OP_UNSPECIFIED && operation == TcpPacketRequestOperation(packet);
}

inline protocol::PushType TcpPacketPushType(const TcpPacket& packet)
{
    if (packet.has_text_message_push())
        return protocol::PushType::PUSH_TYPE_TEXT_MESSAGE;
    if (packet.has_file_message_push())
        return protocol::PushType::PUSH_TYPE_FILE_MESSAGE;
    if (packet.has_friend_request_push())
        return protocol::PushType::PUSH_TYPE_FRIEND_REQUEST;
    if (packet.has_friend_reply_push())
        return protocol::PushType::PUSH_TYPE_FRIEND_REPLY;
    if (packet.has_group_join_request_push())
        return protocol::PushType::PUSH_TYPE_GROUP_JOIN_REQUEST;
    if (packet.has_group_change_push())
        return protocol::PushType::PUSH_TYPE_GROUP_CHANGE;
    if (packet.has_read_receipt_push())
        return protocol::PushType::PUSH_TYPE_READ_RECEIPT;
    return protocol::PushType::PUSH_TYPE_UNSPECIFIED;
}

inline int64_t TcpPacketConversationId(const TcpPacket& packet)
{
    if (packet.has_send_text_request())
        return packet.send_text_request().conversation_id();
    if (packet.has_send_group_text_request())
        return packet.send_group_text_request().conversation_id();
    if (packet.has_send_file_request())
        return packet.send_file_request().conversation_id();
    if (packet.has_send_group_file_request())
        return packet.send_group_file_request().conversation_id();
    if (packet.has_pull_conversation_messages_request())
        return packet.pull_conversation_messages_request().conversation_id();
    if (packet.has_receipt())
        return packet.receipt().conversation_id();
    return 0;
}

inline int64_t TcpPacketReliableSequence(const TcpPacket& packet)
{
    if (packet.has_friend_request_push())
        return packet.friend_request_push().seq();
    if (packet.has_friend_reply_push())
        return packet.friend_reply_push().seq();
    if (packet.has_text_message_push())
        return packet.text_message_push().message_id();
    if (packet.has_file_message_push())
        return packet.file_message_push().message_id();
    if (packet.has_read_receipt_push())
        return packet.read_receipt_push().message_id();
    return 0;
}

inline std::string TcpPacketDebugString(const TcpPacket& packet) { return packet.ShortDebugString(); }

} // namespace wimi
