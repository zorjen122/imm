#include "TcpMessageCodec.h"

#include <iostream>
#include <utility>
#include <vector>

int main()
{
    using imm::TcpPacket;
    using imm::protocol::Operation;

    std::vector<std::pair<Operation, TcpPacket>> requests;
    auto add = [&requests](Operation operation, TcpPacket packet) {
        requests.emplace_back(operation, std::move(packet));
    };

    TcpPacket packet;
    packet.mutable_login_request();
    add(Operation::OP_LOGIN, std::move(packet));
    packet.Clear();
    packet.mutable_pull_friend_list_request();
    add(Operation::OP_PULL_FRIEND_LIST, std::move(packet));
    packet.Clear();
    packet.mutable_pull_friend_apply_list_request();
    add(Operation::OP_PULL_FRIEND_APPLICATIONS, std::move(packet));
    packet.Clear();
    packet.mutable_pull_conversation_messages_request();
    add(Operation::OP_PULL_CONVERSATION_MESSAGES, std::move(packet));
    packet.Clear();
    packet.mutable_pull_group_members_request();
    add(Operation::OP_PULL_GROUP_MEMBERS, std::move(packet));
    packet.Clear();
    packet.mutable_ping_request();
    add(Operation::OP_PING, std::move(packet));
    packet.Clear();
    packet.mutable_quit_request();
    add(Operation::OP_QUIT, std::move(packet));
    packet.Clear();
    packet.mutable_update_user_info_request();
    add(Operation::OP_UPDATE_USER_INFO, std::move(packet));
    packet.Clear();
    packet.mutable_search_user_request();
    add(Operation::OP_SEARCH_USER, std::move(packet));
    packet.Clear();
    packet.mutable_add_friend_request();
    add(Operation::OP_ADD_FRIEND, std::move(packet));
    packet.Clear();
    packet.mutable_reply_friend_request();
    add(Operation::OP_REPLY_FRIEND, std::move(packet));
    packet.Clear();
    packet.mutable_remove_friend_request();
    add(Operation::OP_REMOVE_FRIEND, std::move(packet));
    packet.Clear();
    packet.mutable_send_text_request();
    add(Operation::OP_SEND_TEXT, std::move(packet));
    packet.Clear();
    packet.mutable_send_file_request();
    add(Operation::OP_SEND_FILE, std::move(packet));
    packet.Clear();
    packet.mutable_receipt();
    add(Operation::OP_ACKNOWLEDGE, std::move(packet));
    packet.Clear();
    packet.mutable_create_group_request();
    add(Operation::OP_CREATE_GROUP, std::move(packet));
    packet.Clear();
    packet.mutable_join_group_request();
    add(Operation::OP_JOIN_GROUP, std::move(packet));
    packet.Clear();
    packet.mutable_reply_join_group_request();
    add(Operation::OP_REPLY_JOIN_GROUP, std::move(packet));
    packet.Clear();
    packet.mutable_quit_group_request();
    add(Operation::OP_QUIT_GROUP, std::move(packet));
    packet.Clear();
    packet.mutable_send_group_text_request();
    add(Operation::OP_SEND_GROUP_TEXT, std::move(packet));
    packet.Clear();
    packet.mutable_file_upload_request();
    add(Operation::OP_UPLOAD_FILE, std::move(packet));
    packet.Clear();
    packet.mutable_file_upload_chunk_request();
    add(Operation::OP_UPLOAD_FILE_CHUNK, std::move(packet));
    packet.Clear();
    packet.mutable_file_upload_complete_request();
    add(Operation::OP_COMPLETE_FILE_UPLOAD, std::move(packet));
    packet.Clear();
    packet.mutable_file_download_request();
    add(Operation::OP_DOWNLOAD_FILE, std::move(packet));
    packet.Clear();
    packet.mutable_kick_group_member_request();
    add(Operation::OP_KICK_GROUP_MEMBER, std::move(packet));
    packet.Clear();
    packet.mutable_send_group_file_request();
    add(Operation::OP_SEND_GROUP_FILE, std::move(packet));

    for (const auto& [operation, request] : requests) {
        if (!imm::TcpRequestMatchesOperation(operation, request)) {
            std::cerr << "valid packet rejected for operation " << static_cast<int>(operation) << '\n';
            return 1;
        }
        if (imm::TcpPacketRequestOperation(request) != operation) {
            std::cerr << "request operation inference failed for operation " << static_cast<int>(operation) << '\n';
            return 1;
        }
    }
    if (imm::TcpRequestMatchesOperation(Operation::OP_SEND_TEXT, requests.front().second)) {
        std::cerr << "mismatched business payload accepted\n";
        return 1;
    }

    auto error = imm::MakeErrorPacket(ErrorCodes::ResourceExhausted, "busy");
    if (imm::TcpPacketError(error) != ErrorCodes::ResourceExhausted ||
        error.payload_case() != TcpPacket::PAYLOAD_NOT_SET) {
        std::cerr << "generic response status is invalid\n";
        return 1;
    }

    TcpPacket push;
    push.mutable_text_message_push()->set_message_id(42);
    if (imm::TcpPacketReliableSequence(push) != 42) {
        std::cerr << "push sequence was not read from its payload\n";
        return 1;
    }
    return 0;
}
