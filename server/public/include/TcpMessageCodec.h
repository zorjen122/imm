#pragma once

#include <cstdint>
#include <string>

#include "Const.h"
#include "tcp_message.pb.h"

namespace wimi
{

using TcpPacket = protocol::Packet;

inline bool ParseTcpPacket(const std::string& data, TcpPacket& packet) { return packet.ParseFromString(data); }

inline std::string SerializeTcpPacket(const TcpPacket& packet)
{
    std::string data;
    packet.SerializeToString(&data);
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

inline std::string TcpPacketRequestId(const TcpPacket& packet)
{
    return packet.has_request() ? packet.request().request_id() : std::string{};
}

// 客户端请求的 service_id 与 oneof 载荷必须一一对应。服务端推送虽然复用
// 部分奇数 service_id，但只会出现在出站方向，不参与此入口校验。
inline bool TcpRequestMatchesService(uint32_t serviceId, const TcpPacket& packet)
{
    switch (serviceId) {
    case ID_PULL_FRIEND_LIST_REQ:
        return packet.has_pull_friend_list_request();
    case ID_PULL_FRIEND_APPLY_LIST_REQ:
        return packet.has_pull_friend_apply_list_request();
    case ID_PULL_SESSION_MESSAGE_LIST_REQ:
        return packet.has_pull_conversation_messages_request();
    case ID_GROUP_PULL_MEMBER_REQ:
        return packet.has_pull_group_members_request();
    case ID_PING_REQ:
        return packet.has_ping_request();
    case ID_LOGIN_INIT_REQ:
        return packet.has_login_request();
    case ID_USER_QUIT_REQ:
        return packet.has_quit_request();
    case ID_INIT_USER_INFO_REQ:
        return packet.has_update_user_info_request();
    case ID_SEARCH_USER_REQ:
        return packet.has_search_user_request();
    case ID_NOTIFY_ADD_FRIEND_REQ:
        return packet.has_add_friend_request();
    case ID_REPLY_ADD_FRIEND_REQ:
        return packet.has_reply_friend_request();
    case ID_REMOVE_FRIEND_REQ:
        return packet.has_remove_friend_request();
    case ID_TEXT_SEND_REQ:
        return packet.has_send_text_request();
    case ID_FILE_SEND_REQ:
        return packet.has_send_file_request();
    case ID_FILE_UPLOAD_REQ:
        return packet.has_file_upload_request();
    case ID_ACK:
        return packet.has_receipt();
    case ID_GROUP_CREATE_REQ:
        return packet.has_create_group_request();
    case ID_GROUP_NOTIFY_JOIN_REQ:
        return packet.has_join_group_request();
    case ID_GROUP_REPLY_JOIN_REQ:
        return packet.has_reply_join_group_request();
    case ID_GROUP_QUIT_REQ:
        return packet.has_quit_group_request();
    case ID_GROUP_TEXT_SEND_REQ:
        return packet.has_send_group_text_request();
    case ID_FILE_UPLOAD_CHUNK_REQ:
        return packet.has_file_upload_chunk_request();
    case ID_FILE_UPLOAD_COMPLETE_REQ:
        return packet.has_file_upload_complete_request();
    case ID_FILE_DOWNLOAD_REQ:
        return packet.has_file_download_request();
    case ID_GROUP_KICK_MEMBER_REQ:
        return packet.has_kick_group_member_request();
    case ID_GROUP_FILE_SEND_REQ:
        return packet.has_send_group_file_request();
    default:
        return false;
    }
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

inline int64_t TcpPacketReliableSequence(uint32_t serviceId, const TcpPacket& packet)
{
    switch (serviceId) {
    case ID_NOTIFY_ADD_FRIEND_REQ:
        return packet.has_friend_request_push() ? packet.friend_request_push().seq() : 0;
    case ID_REPLY_ADD_FRIEND_REQ:
        return packet.has_friend_reply_push() ? packet.friend_reply_push().seq() : 0;
    case ID_TEXT_SEND_REQ:
    case ID_GROUP_TEXT_SEND_REQ:
        return packet.has_text_message_push() ? packet.text_message_push().message_id() : 0;
    case ID_FILE_SEND_REQ:
    case ID_GROUP_FILE_SEND_REQ:
        return packet.has_file_message_push() ? packet.file_message_push().message_id() : 0;
    case ID_TEXT_READ_RECEIPT_NOTIFY:
        return packet.has_read_receipt_push() ? packet.read_receipt_push().message_id() : 0;
    default:
        return 0;
    }
}

inline std::string TcpPacketDebugString(const TcpPacket& packet) { return packet.ShortDebugString(); }

} // namespace wimi
