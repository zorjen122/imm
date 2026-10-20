#pragma once
#include "io/mysql_operations.hh"
#include "io/sql_result.hh"

namespace wimi::message_io
{
// 调用者须在整个 execute 期间独占连接；SQL 异常导致连接废弃，不重放结果未知的事务。
class mysql_store
{
    io::mysql_connection& _connection;
    seastar::future<sql_result> query(std::string sql, std::vector<io::mysql_parameter> parameters);

  public:
    explicit mysql_store(io::mysql_connection& connection) : _connection(connection) {}
    seastar::future<db::UserInfo::Ptr> execute(get_user_info operation);
    seastar::future<db::Friend::FriendGroup> execute(get_friend_list operation);
    seastar::future<int> execute(insert_friend_apply operation);
    seastar::future<int> execute(update_friend_apply_status operation);
    seastar::future<int> execute(insert_friend operation);
    seastar::future<db::MessageAcceptResult> execute(accept_direct_text operation);
    seastar::future<int> execute(upsert_ready_file operation);
    seastar::future<int> execute(save_file_upload_progress operation);
    seastar::future<std::optional<db::FileObjectRecord>> execute(find_downloadable_file operation);
    seastar::future<std::optional<db::FileObjectRecord>> execute(find_ready_file operation);
    seastar::future<db::GroupMessageAcceptResult> execute(accept_group_message operation);
    seastar::future<db::ConversationSyncResult> execute(sync_conversation operation);
    seastar::future<db::ConversationAckResult> execute(acknowledge_conversation_message operation);
    seastar::future<db::FriendApply::FriendApplyGroup> execute(get_friend_apply_list operation);
    seastar::future<int> execute(delete_friend operation);
    seastar::future<db::GroupOperationResult> execute(create_group operation);
    seastar::future<db::GroupOperationResult> execute(request_group_join operation);
    seastar::future<db::GroupOperationResult> execute(reply_group_join operation);
    seastar::future<db::GroupOperationResult> execute(remove_group_member operation);
    seastar::future<db::GroupOperationResult> execute(pull_group operation);
    seastar::future<int> execute(update_user_info_name operation);
    seastar::future<int> execute(update_user_info_age operation);
    seastar::future<int> execute(update_user_info_sex operation);
    seastar::future<int> execute(update_user_info_head_image_url operation);
};
} // namespace wimi::message_io
