#include "TcpMessageCodec.h"

#include <iostream>
#include <utility>
#include <vector>

int main()
{
    using wimi::TcpPacket;

    std::vector<std::pair<uint32_t, TcpPacket>> requests;
    auto add = [&requests](uint32_t serviceId, TcpPacket packet) {
        packet.mutable_request()->set_request_id("protocol-test");
        requests.emplace_back(serviceId, std::move(packet));
    };

    TcpPacket packet;
    packet.mutable_login_request();
    add(ID_LOGIN_INIT_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_pull_friend_list_request();
    add(ID_PULL_FRIEND_LIST_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_pull_friend_apply_list_request();
    add(ID_PULL_FRIEND_APPLY_LIST_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_pull_conversation_messages_request();
    add(ID_PULL_SESSION_MESSAGE_LIST_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_pull_group_members_request();
    add(ID_GROUP_PULL_MEMBER_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_ping_request();
    add(ID_PING_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_quit_request();
    add(ID_USER_QUIT_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_update_user_info_request();
    add(ID_INIT_USER_INFO_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_search_user_request();
    add(ID_SEARCH_USER_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_add_friend_request();
    add(ID_NOTIFY_ADD_FRIEND_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_reply_friend_request();
    add(ID_REPLY_ADD_FRIEND_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_remove_friend_request();
    add(ID_REMOVE_FRIEND_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_send_text_request();
    add(ID_TEXT_SEND_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_send_file_request();
    add(ID_FILE_SEND_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_receipt();
    add(ID_ACK, std::move(packet));
    packet.Clear();
    packet.mutable_create_group_request();
    add(ID_GROUP_CREATE_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_join_group_request();
    add(ID_GROUP_NOTIFY_JOIN_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_reply_join_group_request();
    add(ID_GROUP_REPLY_JOIN_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_quit_group_request();
    add(ID_GROUP_QUIT_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_send_group_text_request();
    add(ID_GROUP_TEXT_SEND_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_file_upload_request();
    add(ID_FILE_UPLOAD_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_file_upload_chunk_request();
    add(ID_FILE_UPLOAD_CHUNK_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_file_upload_complete_request();
    add(ID_FILE_UPLOAD_COMPLETE_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_file_download_request();
    add(ID_FILE_DOWNLOAD_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_kick_group_member_request();
    add(ID_GROUP_KICK_MEMBER_REQ, std::move(packet));
    packet.Clear();
    packet.mutable_send_group_file_request();
    add(ID_GROUP_FILE_SEND_REQ, std::move(packet));

    for (const auto& [serviceId, request] : requests) {
        if (!wimi::TcpRequestMatchesService(serviceId, request)) {
            std::cerr << "valid packet rejected for service " << serviceId << '\n';
            return 1;
        }
        if (wimi::TcpPacketRequestId(request) != "protocol-test") {
            std::cerr << "request context lost for service " << serviceId << '\n';
            return 1;
        }
    }
    if (wimi::TcpRequestMatchesService(ID_TEXT_SEND_REQ, requests.front().second)) {
        std::cerr << "mismatched business payload accepted\n";
        return 1;
    }

    auto error = wimi::MakeErrorPacket(ErrorCodes::ResourceExhausted, "busy");
    if (wimi::TcpPacketError(error) != ErrorCodes::ResourceExhausted ||
        error.payload_case() != TcpPacket::PAYLOAD_NOT_SET) {
        std::cerr << "generic response status is invalid\n";
        return 1;
    }

    TcpPacket push;
    push.mutable_text_message_push()->set_message_id(42);
    if (wimi::TcpPacketReliableSequence(ID_TEXT_SEND_REQ, push) != 42 ||
        wimi::TcpPacketReliableSequence(ID_NOTIFY_ADD_FRIEND_REQ, push) != 0) {
        std::cerr << "push sequence was read from the wrong business payload\n";
        return 1;
    }
    return 0;
}
