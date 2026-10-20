#pragma once
#include "message_records.hh"
#include "reactor/mysql_connection.hh"
#include <variant>

namespace wimi::message_io
{
struct get_user_info {
    using result_type = db::UserInfo::Ptr;
    long uid{};
    size_t byte_size() const { return sizeof(*this); }
};
struct get_friend_list {
    using result_type = db::Friend::FriendGroup;
    long uid{};
    size_t byte_size() const { return sizeof(*this); }
};
struct insert_friend_apply {
    using result_type = int;
    db::FriendApply::Ptr friend_data{};
    size_t byte_size() const
    {
        return sizeof(*this) +
               (friend_data ? sizeof(db::FriendApply) + friend_data->content.size() + friend_data->createTime.size()
                            : 0);
    }
};
struct update_friend_apply_status {
    using result_type = int;
    db::FriendApply::Ptr friend_data{};
    size_t byte_size() const
    {
        return sizeof(*this) +
               (friend_data ? sizeof(db::FriendApply) + friend_data->content.size() + friend_data->createTime.size()
                            : 0);
    }
};
struct insert_friend {
    using result_type = int;
    db::Friend::Ptr friend_data{};
    size_t byte_size() const
    {
        return sizeof(*this) + (friend_data ? sizeof(db::Friend) + friend_data->createTime.size() : 0);
    }
};
struct accept_direct_text {
    using result_type = db::MessageAcceptResult;
    long sender_id{};
    long receiver_id{};
    std::string client_message_id{};
    std::string content{};
    std::string send_date_time{};
    int message_type{db::Message::Type::TEXT};
    size_t byte_size() const
    {
        return sizeof(*this) + client_message_id.size() + content.size() + send_date_time.size();
    }
};
struct upsert_ready_file {
    using result_type = int;
    db::FileObjectRecord file{};
    std::string created_time{};
    std::string expires_time{};
    size_t byte_size() const
    {
        return sizeof(*this) + file.fileId.size() + file.fileName.size() + file.fileType.size() + file.sha256.size() +
               created_time.size() + expires_time.size();
    }
};
struct save_file_upload_progress {
    using result_type = int;
    long owner_uid{};
    std::string file_id{};
    int64_t next_offset{};
    bool complete{};
    std::string update_time{};
    size_t byte_size() const { return sizeof(*this) + file_id.size() + update_time.size(); }
};
struct find_downloadable_file {
    using result_type = std::optional<db::FileObjectRecord>;
    std::string file_id{};
    long uid{};
    size_t byte_size() const { return sizeof(*this) + file_id.size(); }
};
struct find_ready_file {
    using result_type = std::optional<db::FileObjectRecord>;
    std::string file_id{};
    long owner_uid{};
    size_t byte_size() const { return sizeof(*this) + file_id.size(); }
};
struct accept_group_message {
    using result_type = db::GroupMessageAcceptResult;
    long sender_id{};
    long group_id{};
    std::string client_message_id{};
    std::string content{};
    int message_type{};
    std::string send_date_time{};
    bool require_ready_owned_file{false};
    size_t byte_size() const
    {
        return sizeof(*this) + client_message_id.size() + content.size() + send_date_time.size();
    }
};
struct sync_conversation {
    using result_type = db::ConversationSyncResult;
    long uid{};
    long conversation_id{};
    long after_seq{};
    int pull_count{};
    size_t byte_size() const { return sizeof(*this); }
};
struct acknowledge_conversation_message {
    using result_type = db::ConversationAckResult;
    long message_id{};
    long uid{};
    std::string device_id{};
    long conversation_id{};
    long conversation_seq{};
    short status{};
    std::string read_date_time{""};
    size_t byte_size() const { return sizeof(*this) + device_id.size() + read_date_time.size(); }
};
struct get_friend_apply_list {
    using result_type = db::FriendApply::FriendApplyGroup;
    long from{};
    size_t byte_size() const { return sizeof(*this); }
};
struct delete_friend {
    using result_type = int;
    long uid_a{};
    long uid_b{};
    size_t byte_size() const { return sizeof(*this); }
};
struct create_group {
    using result_type = db::GroupOperationResult;
    db::GroupManager group{};
    size_t byte_size() const { return sizeof(*this) + group.name.size() + group.createTime.size(); }
};
struct request_group_join {
    using result_type = db::GroupOperationResult;
    long group_id{};
    long uid{};
    std::string message{};
    std::string now{};
    size_t byte_size() const { return sizeof(*this) + message.size() + now.size(); }
};
struct reply_group_join {
    using result_type = db::GroupOperationResult;
    long group_id{};
    long actor_uid{};
    long requestor_uid{};
    bool accept{};
    std::string now{};
    size_t byte_size() const { return sizeof(*this) + now.size(); }
};
struct remove_group_member {
    using result_type = db::GroupOperationResult;
    long group_id{};
    long actor_uid{};
    long target_uid{};
    bool dissolve_if_master{};
    size_t byte_size() const { return sizeof(*this); }
};
struct pull_group {
    using result_type = db::GroupOperationResult;
    long group_id{};
    long uid{};
    size_t byte_size() const { return sizeof(*this); }
};
struct update_user_info_name {
    using result_type = int;
    long uid{};
    std::string name{};
    size_t byte_size() const { return sizeof(*this) + name.size(); }
};
struct update_user_info_age {
    using result_type = int;
    long uid{};
    int age{};
    size_t byte_size() const { return sizeof(*this); }
};
struct update_user_info_sex {
    using result_type = int;
    long uid{};
    std::string sex{};
    size_t byte_size() const { return sizeof(*this) + sex.size(); }
};
struct update_user_info_head_image_url {
    using result_type = int;
    long uid{};
    std::string head_image_url{};
    size_t byte_size() const { return sizeof(*this) + head_image_url.size(); }
};
using mysql_operations =
    std::variant<get_user_info, get_friend_list, insert_friend_apply, update_friend_apply_status, insert_friend,
                 accept_direct_text, upsert_ready_file, save_file_upload_progress, find_downloadable_file,
                 find_ready_file, accept_group_message, sync_conversation, acknowledge_conversation_message,
                 get_friend_apply_list, delete_friend, create_group, request_group_join, reply_group_join,
                 remove_group_member, pull_group, update_user_info_name, update_user_info_age, update_user_info_sex,
                 update_user_info_head_image_url>;
} // namespace wimi::message_io
