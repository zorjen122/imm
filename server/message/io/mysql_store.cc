#include "io/mysql_store.hh"
#include "Logger.h"
#include <seastar/core/coroutine.hh>
#include <algorithm>

namespace imm::message_io
{
using namespace db;
seastar::future<sql_result> mysql_store::query(std::string sql, std::vector<io::mysql_parameter> arguments)
{
    auto result = co_await _connection.query_rows(std::move(sql), std::move(arguments));
    if (result.error) {
        throw std::runtime_error(result.diagnostic);
    }
    co_return sql_result(std::move(result));
}
seastar::future<db::UserInfo::Ptr> mysql_store::execute(get_user_info operation)
{
    auto& uid = operation.uid;

    auto result = (co_await query("SELECT * FROM userInfo WHERE uid = ?", parameters(uid)));
    if (!(result.rows.size() > 0)) {
        LOG_DEBUG(logger, "Mysql指令调用的结果集为空");
        co_return nullptr;
    }
    auto row = result.first_row();
    co_return std::make_shared<UserInfo>(uid,
                                         field<std::string>(row[1]), // name
                                         field<int>(row[2]),         // age
                                         field<std::string>(row[3]), // sex
                                         field<std::string>(row[4])  // headImageURL
    );
}
seastar::future<db::Friend::FriendGroup> mysql_store::execute(get_friend_list operation)
{
    auto& uid = operation.uid;

    auto result = (co_await query(
        R"(SELECT f.friendshipId, c.conversationId, ?, CASE WHEN f.uidA = ? THEN f.uidB ELSE f.uidA END, f.createTime FROM friends f INNER JOIN conversations c ON c.type = 1 AND c.businessId = f.friendshipId WHERE f.uidA = ? OR f.uidB = ?)",
        parameters(uid, uid, uid, uid)));

    if (!(result.rows.size() > 0)) {
        LOG_DEBUG(logger, "Mysql指令调用的结果集为空");
        co_return nullptr;
    }

    auto friend_group = std::make_shared<std::vector<Friend::Ptr>>();
    for (const auto& row : result.rows) {
        friend_group->emplace_back(new Friend(field<size_t>(row[0]),     // friendshipId
                                              field<size_t>(row[1]),     // conversationId
                                              field<size_t>(row[2]),     // requested uid
                                              field<size_t>(row[3]),     // peer uid
                                              field<std::string>(row[4]) // createTime
                                              ));
    }
    co_return friend_group;
}
seastar::future<int> mysql_store::execute(insert_friend_apply operation)
{
    auto& friend_data = operation.friend_data;

    std::string f = R"(INSERT INTO friendApplys VALUES (?, ?, ?, ?, ?))";
    // 存两份，便于两边查找
    auto result = (co_await query(f, parameters(friend_data->from, friend_data->to, friend_data->content,
                                                friend_data->status, friend_data->createTime)));
    result = (co_await query(f, parameters(friend_data->to, friend_data->from, friend_data->content,
                                           friend_data->status, friend_data->createTime)));
    co_return 0;
}
seastar::future<int> mysql_store::execute(update_friend_apply_status operation)
{
    auto& friend_data = operation.friend_data;

    // update: status, content, createTime
    std::string f =
        R"(UPDATE friendApplys SET status = ?, content = ?, createTime = ? WHERE fromUid = ? AND toUid = ?)";
    auto result = (co_await query(f, parameters(friend_data->status, friend_data->content, friend_data->createTime,
                                                friend_data->from, friend_data->to)));

    result = (co_await query(f, parameters(friend_data->status, friend_data->content, friend_data->createTime,
                                           friend_data->to, friend_data->from)));
    co_return 0;
}
seastar::future<int> mysql_store::execute(insert_friend operation)
{
    auto& friend_data = operation.friend_data;

    std::string date_time = getCurrentDateTime();
    const auto uid_a = std::min(friend_data->uidA, friend_data->uidB);
    const auto uid_b = std::max(friend_data->uidA, friend_data->uidB);
    co_await _connection.command("START TRANSACTION");
    try {
        (co_await query(R"(INSERT INTO friends (friendshipId, uidA, uidB, createTime) VALUES (?, ?, ?, ?))",
                        parameters(friend_data->friendshipId, uid_a, uid_b, date_time)));
        (co_await query(
            R"(INSERT INTO conversations (conversationId, type, businessId, latestSeq, createTime) VALUES (?, 1, ?, 0, ?))",
            parameters(friend_data->conversationId, friend_data->friendshipId, date_time)));
        (co_await query(
            R"(INSERT INTO conversationUserStates (conversationId, uid, joinedSeq) VALUES (?, ?, 1), (?, ?, 1))",
            parameters(friend_data->conversationId, uid_a, friend_data->conversationId, uid_b)));
        co_await _connection.command("COMMIT");
        co_return 0;
    } catch (...) {
        try {
            _connection.abort();
        } catch (...) {
        }
        throw;
    }
}
seastar::future<db::MessageAcceptResult> mysql_store::execute(accept_direct_text operation)
{
    auto& sender_id = operation.sender_id;
    auto& receiver_id = operation.receiver_id;
    auto& client_message_id = operation.client_message_id;
    auto& content = operation.content;
    auto& send_date_time = operation.send_date_time;
    auto& message_type = operation.message_type;

    MessageAcceptResult accepted;
    co_await _connection.command("START TRANSACTION");
    try {
        auto friend_result = (co_await query(
            R"(SELECT c.conversationId FROM friends f INNER JOIN conversations c ON c.type = 1 AND c.businessId = f.friendshipId WHERE f.uidA = ? AND f.uidB = ?)",
            parameters(std::min(sender_id, receiver_id), std::max(sender_id, receiver_id))));
        auto friend_row = friend_result.first_row();
        if (!friend_row) {
            co_await _connection.command("ROLLBACK");
            accepted.error = ErrorCodes::UserNotFriend;
            co_return accepted;
        }

        accepted.conversationId = field<int64_t>(friend_row[0]);
        (co_await query(
            R"(INSERT IGNORE INTO conversationUserStates (conversationId, uid, joinedSeq) VALUES (?, ?, 1), (?, ?, 1))",
            parameters(accepted.conversationId, sender_id, accepted.conversationId, receiver_id)));

        auto sequence_result =
            (co_await query(R"(SELECT latestSeq FROM conversations WHERE conversationId = ? FOR UPDATE)",
                            parameters(accepted.conversationId)));
        auto sequence_row = sequence_result.first_row();
        if (!sequence_row) {
            co_await _connection.command("ROLLBACK");
            accepted.error = ErrorCodes::MysqlFailed;
            accepted.diagnostic = "conversation row missing after insert";
            co_return accepted;
        }

        auto duplicate_result = (co_await query(
            R"(SELECT m.messageId,
                                      m.conversationId,
                                      m.conversationSeq,
                                      m.receiverId,
                                      m.type,
                                      m.content
                               FROM messages m
                               WHERE m.senderId = ? AND m.clientMessageId = ?)",
            parameters(sender_id, client_message_id)));
        auto duplicate_row = duplicate_result.first_row();
        if (duplicate_row) {
            const bool same_command = field<int64_t>(duplicate_row[1]) == accepted.conversationId &&
                                      field<int64_t>(duplicate_row[3]) == receiver_id &&
                                      field<int>(duplicate_row[4]) == message_type &&
                                      field<std::string>(duplicate_row[5]) == content;
            if (!same_command) {
                co_await _connection.command("ROLLBACK");
                accepted.error = ErrorCodes::IdempotencyConflict;
                co_return accepted;
            }
            accepted.messageId = field<int64_t>(duplicate_row[0]);
            accepted.conversationSeq = field<int64_t>(duplicate_row[2]);
            accepted.duplicate = true;
            accepted.error = ErrorCodes::Success;
            co_await _connection.command("COMMIT");
            co_return accepted;
        }

        accepted.conversationSeq = field<int64_t>(sequence_row[0]) + 1;
        (co_await query(R"(UPDATE conversations SET latestSeq = ? WHERE conversationId = ?)",
                        parameters(accepted.conversationSeq, accepted.conversationId)));

        const std::string canonical_command =
            std::to_string(accepted.conversationId) + ":" + std::to_string(message_type) + ":" + content;
        const std::string command_hash = std::to_string(std::hash<std::string>{}(canonical_command));
        auto insert_result = (co_await query(
            R"(INSERT INTO messages (senderId, receiverId, conversationId, conversationSeq, clientMessageId, commandHash, type, content, status, sendDateTime, readDateTime) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ''))",
            parameters(sender_id, receiver_id, accepted.conversationId, accepted.conversationSeq, client_message_id,
                       command_hash, message_type, content, static_cast<int>(Message::Status::WAIT), send_date_time)));
        accepted.messageId = static_cast<int64_t>(insert_result.insert_id);
        co_await _connection.command("COMMIT");
        accepted.error = ErrorCodes::Success;
        co_return accepted;
    } catch (const std::exception& error) {
        try {
            _connection.abort();
        } catch (...) {
        }
        accepted.error = ErrorCodes::MysqlFailed;
        accepted.diagnostic = error.what();
        co_return accepted;
    }
}
seastar::future<int> mysql_store::execute(upsert_ready_file operation)
{
    auto& file = operation.file;
    auto& created_time = operation.created_time;
    auto& expires_time = operation.expires_time;

    auto existing =
        (co_await query(R"(SELECT ownerUid FROM fileObjects WHERE fileId = ? FOR UPDATE)", parameters(file.fileId)))
            .first_row();
    if (existing && field<long>(existing[0]) != file.ownerUid) {
        co_return 1;
    }

    (co_await query(
        R"(INSERT INTO fileObjects
                           (fileId, ownerUid, fileName, fileType, fileSize, sha256, state, createdTime, expiresTime)
                       VALUES (?, ?, ?, ?, ?, ?, 1, ?, ?)
                       ON DUPLICATE KEY UPDATE
                           fileName = VALUES(fileName),
                           fileType = VALUES(fileType),
                           fileSize = VALUES(fileSize),
                           sha256 = VALUES(sha256),
                           state = 1,
                           expiresTime = VALUES(expiresTime))",
        parameters(file.fileId, file.ownerUid, file.fileName, file.fileType, file.fileSize,
                   std::string(reinterpret_cast<const char*>(file.sha256.data()), file.sha256.size()), created_time,
                   expires_time)));
    co_return 0;
}
seastar::future<int> mysql_store::execute(save_file_upload_progress operation)
{
    auto& owner_uid = operation.owner_uid;
    auto& file_id = operation.file_id;
    auto& next_offset = operation.next_offset;
    auto& complete = operation.complete;
    auto& update_time = operation.update_time;

    (co_await query(
        R"(INSERT INTO fileUploads (ownerUid, fileId, nextOffset, complete, updateTime)
                       VALUES (?, ?, ?, ?, ?)
                       ON DUPLICATE KEY UPDATE
                           nextOffset = VALUES(nextOffset),
                           complete = VALUES(complete),
                           updateTime = VALUES(updateTime))",
        parameters(owner_uid, file_id, next_offset, complete ? 1 : 0, update_time)));
    co_return 0;
}
seastar::future<std::optional<db::FileObjectRecord>> mysql_store::execute(find_downloadable_file operation)
{
    auto& file_id = operation.file_id;
    auto& uid = operation.uid;

    auto result = (co_await query(
        R"(SELECT f.fileId, f.ownerUid, f.fileName, f.fileType, f.fileSize, f.sha256, f.state
                           FROM fileObjects f
                           INNER JOIN messages m
                               ON m.content = f.fileId AND m.type = 5
                           INNER JOIN conversationUserStates s
                               ON s.conversationId = m.conversationId AND s.uid = ?
                           WHERE f.fileId = ? AND f.state = 1 AND f.expiresTime > ?
                             AND (NOT EXISTS (SELECT 1 FROM conversations gc WHERE gc.conversationId = m.conversationId AND gc.type = 2)
                                  OR EXISTS (SELECT 1 FROM groupInfo gi INNER JOIN groupMembers gm ON gm.groupId = gi.groupId AND gm.uid = ? WHERE gi.conversationId = m.conversationId))
                           LIMIT 1)",
        parameters(uid, file_id, getCurrentDateTime(), uid)));
    auto row = result.first_row();
    if (!row) {
        co_return std::nullopt;
    }

    FileObjectRecord file;
    file.fileId = field<std::string>(row[0]);
    file.ownerUid = field<long>(row[1]);
    file.fileName = field<std::string>(row[2]);
    file.fileType = field<std::string>(row[3]);
    file.fileSize = field<int64_t>(row[4]);

    const auto sha256 = field<std::string>(row[5]);
    file.sha256.assign(sha256.data(), sha256.size());
    co_return file;
}
seastar::future<std::optional<db::FileObjectRecord>> mysql_store::execute(find_ready_file operation)
{
    auto& file_id = operation.file_id;
    auto& owner_uid = operation.owner_uid;

    auto result = (co_await query(
        R"(SELECT fileId, ownerUid, fileName, fileType, fileSize, sha256, state
                           FROM fileObjects
                           WHERE fileId = ? AND ownerUid = ? AND state = 1 AND expiresTime > ?)",
        parameters(file_id, owner_uid, getCurrentDateTime())));
    auto row = result.first_row();
    if (!row) {
        co_return std::nullopt;
    }

    FileObjectRecord file;
    file.fileId = field<std::string>(row[0]);
    file.ownerUid = field<long>(row[1]);
    file.fileName = field<std::string>(row[2]);
    file.fileType = field<std::string>(row[3]);
    file.fileSize = field<int64_t>(row[4]);

    const auto sha256 = field<std::string>(row[5]);
    file.sha256.assign(sha256.data(), sha256.size());
    co_return file;
}
seastar::future<db::GroupMessageAcceptResult> mysql_store::execute(accept_group_message operation)
{
    auto& sender_id = operation.sender_id;
    auto& group_id = operation.group_id;
    auto& client_message_id = operation.client_message_id;
    auto& content = operation.content;
    auto& message_type = operation.message_type;
    auto& send_date_time = operation.send_date_time;
    auto& require_ready_owned_file = operation.require_ready_owned_file;

    GroupMessageAcceptResult accepted;
    accepted.groupId = group_id;
    co_await _connection.command("START TRANSACTION");
    try {
        auto group_result = (co_await query(
            R"(SELECT gi.conversationId FROM groupInfo gi INNER JOIN groupMembers gm ON gm.groupId = gi.groupId WHERE gi.groupId = ? AND gm.uid = ? FOR UPDATE)",
            parameters(group_id, sender_id)));
        auto group_row = group_result.first_row();
        if (!group_row) {
            co_await _connection.command("ROLLBACK");
            accepted.error = ErrorCodes::MessageOwnershipInvalid;
            co_return accepted;
        }
        accepted.conversationId = field<int64_t>(group_row[0]);
        if (require_ready_owned_file &&
            !(co_await query(
                  R"(SELECT 1 FROM fileObjects WHERE fileId = ? AND ownerUid = ? AND state = 1 AND expiresTime > ? FOR UPDATE)",
                  parameters(content, sender_id, send_date_time)))
                 .first_row()) {
            co_await _connection.command("ROLLBACK");
            accepted.error = ErrorCodes::NotFound;
            co_return accepted;
        }

        auto sequence_result =
            (co_await query(R"(SELECT latestSeq FROM conversations WHERE conversationId = ? FOR UPDATE)",
                            parameters(accepted.conversationId)));
        auto sequence_row = sequence_result.first_row();
        if (!sequence_row) {
            co_await _connection.command("ROLLBACK");
            accepted.error = ErrorCodes::MysqlFailed;
            accepted.diagnostic = "group conversation row missing after insert";
            co_return accepted;
        }

        auto duplicate_result = (co_await query(
            R"(SELECT messageId, conversationId, conversationSeq, receiverId, type, content FROM messages WHERE senderId = ? AND clientMessageId = ?)",
            parameters(sender_id, client_message_id)));
        auto duplicate_row = duplicate_result.first_row();
        if (duplicate_row) {
            const bool same_command = field<int64_t>(duplicate_row[1]) == accepted.conversationId &&
                                      field<int64_t>(duplicate_row[3]) == group_id &&
                                      field<int>(duplicate_row[4]) == message_type &&
                                      field<std::string>(duplicate_row[5]) == content;
            if (!same_command) {
                co_await _connection.command("ROLLBACK");
                accepted.error = ErrorCodes::IdempotencyConflict;
                co_return accepted;
            }
            accepted.messageId = field<int64_t>(duplicate_row[0]);
            accepted.conversationSeq = field<int64_t>(duplicate_row[2]);
            accepted.duplicate = true;
        } else {
            accepted.conversationSeq = field<int64_t>(sequence_row[0]) + 1;
            (co_await query(R"(UPDATE conversations SET latestSeq = ? WHERE conversationId = ?)",
                            parameters(accepted.conversationSeq, accepted.conversationId)));
            const std::string canonical_command =
                std::to_string(accepted.conversationId) + ":" + std::to_string(message_type) + ":" + content;
            auto insert_result = (co_await query(
                R"(INSERT INTO messages (senderId, receiverId, conversationId, conversationSeq, clientMessageId, commandHash, type, content, status, sendDateTime, readDateTime) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ''))",
                parameters(sender_id, group_id, accepted.conversationId, accepted.conversationSeq, client_message_id,
                           std::to_string(std::hash<std::string>{}(canonical_command)), message_type, content,
                           static_cast<int>(Message::Status::WAIT), send_date_time)));
            accepted.messageId = static_cast<int64_t>(insert_result.insert_id);
        }

        auto members = (co_await query(R"(SELECT uid FROM groupMembers WHERE groupId = ? AND uid <> ?)",
                                       parameters(group_id, sender_id)));
        for (const auto& row : members.rows) {
            accepted.recipientUids.push_back(field<int64_t>(row[0]));
        }
        co_await _connection.command("COMMIT");
        accepted.error = ErrorCodes::Success;
        co_return accepted;
    } catch (const std::exception& error) {
        try {
            _connection.abort();
        } catch (...) {
        }
        accepted.error = ErrorCodes::MysqlFailed;
        accepted.diagnostic = error.what();
        co_return accepted;
    }
}
seastar::future<db::ConversationSyncResult> mysql_store::execute(sync_conversation operation)
{
    auto& uid = operation.uid;
    auto& conversation_id = operation.conversation_id;
    auto& after_seq = operation.after_seq;
    auto& pull_count = operation.pull_count;

    if (uid <= 0 || conversation_id <= 0 || after_seq < 0) {
        co_return ConversationSyncResult{ErrorCodes::JsonParser};
    }
    const int bounded_count = std::clamp(pull_count > 0 ? pull_count : 50, 1, 100);

    ConversationSyncResult sync;
    auto membership = (co_await query(
        R"(SELECT c.latestSeq, m.joinedSeq, c.latestSeq, c.type
                           FROM conversations c
                           INNER JOIN conversationUserStates m ON m.conversationId = c.conversationId
                           WHERE c.conversationId = ? AND m.uid = ?
                             AND (c.type <> 2 OR EXISTS (
                                 SELECT 1 FROM groupInfo gi
                                 INNER JOIN groupMembers gm ON gm.groupId = gi.groupId AND gm.uid = ?
                                 WHERE gi.conversationId = c.conversationId)))",
        parameters(conversation_id, uid, uid)));
    auto member_row = membership.first_row();
    if (!member_row) {
        sync.error = ErrorCodes::MessageOwnershipInvalid;
        co_return sync;
    }

    sync.latestSeq = field<int64_t>(member_row[0]);
    const int64_t joined_seq = field<int64_t>(member_row[1]);
    const int64_t left_seq = field<int64_t>(member_row[2]);
    sync.conversationType = field<int>(member_row[3]);
    const int64_t effective_after = std::max(after_seq, joined_seq - 1);
    auto result = (co_await query(
        R"(SELECT m.messageId,
                                  m.senderId,
                                  m.receiverId,
                                  m.conversationId,
                                  m.conversationSeq,
                                  COALESCE(m.clientMessageId, ''),
                                  m.type,
                                  m.content,
                                  m.status,
                                  m.sendDateTime,
                                  m.readDateTime,
                                  COALESCE(f.fileId, ''),
                                  COALESCE(f.fileName, ''),
                                  COALESCE(f.fileType, ''),
                                  CAST(COALESCE(f.fileSize, 0) AS SIGNED),
                                  f.sha256,
                                  CASE
                                      WHEN f.fileId IS NOT NULL
                                           AND (f.state <> 1 OR f.expiresTime <= ?) THEN 1
                                      ELSE 0
                                  END
                           FROM messages m
                           LEFT JOIN fileObjects f
                               ON m.type = 5 AND f.fileId = m.content
                           WHERE m.conversationId = ?
                             AND m.conversationSeq > ?
                             AND m.conversationSeq <= ?
                           ORDER BY m.conversationSeq ASC
                           LIMIT ?)",
        parameters(getCurrentDateTime(), conversation_id, effective_after, left_seq, bounded_count + 1)));
    for (const auto& row : result.rows) {
        if (sync.messages.size() >= static_cast<std::size_t>(bounded_count)) {
            sync.hasMore = true;
            break;
        }
        ConversationMessageRecord message;
        message.messageId = field<int64_t>(row[0]);
        message.senderId = field<int64_t>(row[1]);
        message.receiverId = field<int64_t>(row[2]);
        message.conversationId = field<int64_t>(row[3]);
        message.conversationSeq = field<int64_t>(row[4]);
        message.clientMessageId = field<std::string>(row[5]);
        message.type = field<int>(row[6]);
        message.content = field<std::string>(row[7]);
        message.status = field<int>(row[8]);
        message.sendDateTime = field<std::string>(row[9]);
        message.readDateTime = field<std::string>(row[10]);

        if (!field<std::string>(row[11]).empty()) {
            ConversationMessageRecord::FileAttachment attachment;
            attachment.fileId = field<std::string>(row[11]);
            attachment.fileName = field<std::string>(row[12]);
            attachment.fileType = field<std::string>(row[13]);
            attachment.fileSize = field<int64_t>(row[14]);

            const auto sha256 = field<std::string>(row[15]);
            attachment.sha256.assign(sha256.data(), sha256.size());
            attachment.expired = field<int64_t>(row[16]) != 0;
            message.file = std::move(attachment);
        }
        sync.nextSeq = message.conversationSeq;
        sync.messages.push_back(std::move(message));
    }
    if (sync.messages.empty()) {
        sync.nextSeq = effective_after;
    }
    sync.error = ErrorCodes::Success;
    co_return sync;
}
seastar::future<db::ConversationAckResult> mysql_store::execute(acknowledge_conversation_message operation)
{
    auto& message_id = operation.message_id;
    auto& uid = operation.uid;
    auto& device_id = operation.device_id;
    auto& conversation_id = operation.conversation_id;
    auto& conversation_seq = operation.conversation_seq;
    auto& status = operation.status;
    auto& read_date_time = operation.read_date_time;

    if (message_id <= 0 || uid <= 0 || device_id.empty() || conversation_id <= 0 || conversation_seq <= 0) {
        co_return {.updated = 0};
    }

    ConversationAckResult acknowledged;
    co_await _connection.command("START TRANSACTION");
    try {
        auto owner = (co_await query(
            R"(SELECT c.type, msg.receiverId, msg.senderId, msg.status FROM messages msg INNER JOIN conversations c ON c.conversationId = msg.conversationId INNER JOIN conversationUserStates cm ON cm.conversationId = c.conversationId AND cm.uid = ? INNER JOIN devices d ON d.uid = cm.uid AND d.deviceId = ? WHERE msg.messageId = ? AND msg.conversationId = ? AND msg.conversationSeq = ? FOR UPDATE)",
            parameters(uid, device_id, message_id, conversation_id, conversation_seq)));
        auto owner_row = owner.first_row();
        if (!owner_row) {
            co_await _connection.command("ROLLBACK");
            acknowledged.updated = 0;
            co_return acknowledged;
        }
        const int conversation_type = field<int>(owner_row[0]);
        const bool direct_recipient = conversation_type == 1 && field<int64_t>(owner_row[1]) == uid;
        acknowledged.senderUid = field<int64_t>(owner_row[2]);
        acknowledged.directConversation = conversation_type == 1;
        acknowledged.readStateChanged =
            direct_recipient && status == Message::Status::READ && field<int>(owner_row[3]) < Message::Status::READ;

        if (direct_recipient) {
            (co_await query(
                R"(UPDATE messages SET readDateTime = CASE WHEN ? = 3 AND (status < 3 OR readDateTime IS NULL OR readDateTime = '') THEN ? ELSE readDateTime END, status = GREATEST(status, ?) WHERE messageId = ?)",
                parameters(status, read_date_time, status, message_id)));
        }
        (co_await query(
            R"(UPDATE conversationUserStates SET deliveredSeq = GREATEST(deliveredSeq, ?), readSeq = CASE WHEN ? = 3 THEN GREATEST(readSeq, ?) ELSE readSeq END WHERE uid = ? AND conversationId = ?)",
            parameters(conversation_seq, status, conversation_seq, uid, conversation_id)));
        (co_await query(
            R"(INSERT INTO conversationDeviceStates (conversationId, deviceId, persistedSeq) VALUES (?, ?, ?) ON DUPLICATE KEY UPDATE persistedSeq = GREATEST(persistedSeq, VALUES(persistedSeq)))",
            parameters(conversation_id, device_id, conversation_seq)));
        co_await _connection.command("COMMIT");
        acknowledged.updated = 1;
        co_return acknowledged;
    } catch (...) {
        try {
            _connection.abort();
        } catch (...) {
        }
        throw;
    }
}
seastar::future<db::FriendApply::FriendApplyGroup> mysql_store::execute(get_friend_apply_list operation)
{
    auto& from = operation.from;

    const static short STATUS = 0;
    std::string f = R"(SELECT * FROM friendApplys WHERE status = ? AND fromUid = ?)";
    auto result = (co_await query(f, parameters(STATUS, from)));
    if (!(result.rows.size() > 0)) {
        // log...
        LOG_DEBUG(logger, "Mysql指令调用的结果集为空");
        co_return nullptr;
    }
    FriendApply::FriendApplyGroup friend_apply_group(new std::vector<FriendApply::Ptr>());
    FriendApply::Ptr friend_apply;
    for (auto row : result.rows) {
        size_t from_uid = field<size_t>(row[0]);
        size_t to_uid = field<size_t>(row[1]);
        std::string content = field<std::string>(row[2]);
        short status = field<int>(row[3]);
        std::string create_time = field<std::string>(row[4]);
        friend_apply.reset(new FriendApply(from_uid, to_uid, status, content, create_time));
        friend_apply_group->push_back(friend_apply);
    }
    co_return friend_apply_group;
}
seastar::future<int> mysql_store::execute(delete_friend operation)
{
    auto& uid_a = operation.uid_a;
    auto& uid_b = operation.uid_b;

    auto result = (co_await query(R"(DELETE FROM friends WHERE uidA = ? AND uidB = ?)",
                                  parameters(std::min(uid_a, uid_b), std::max(uid_a, uid_b))));
    co_return 0;
}
seastar::future<db::GroupOperationResult> mysql_store::execute(create_group operation)
{
    auto& group = operation.group;

    GroupOperationResult result;
    result.groupId = group.groupId;
    result.conversationId = group.conversationId;
    result.masterUid = group.masterUid;
    result.targetUid = group.masterUid;
    result.groupName = group.name;
    co_await _connection.command("START TRANSACTION");
    try {
        (co_await query(
            R"(INSERT INTO groupInfo (groupId, masterUid, name, conversationId, createTime) VALUES (?, ?, ?, ?, ?))",
            parameters(group.groupId, group.masterUid, group.name, group.conversationId, group.createTime)));
        (co_await query(
            R"(INSERT INTO conversations (conversationId, type, businessId, latestSeq, createTime) VALUES (?, 2, ?, 0, ?))",
            parameters(group.conversationId, group.groupId, group.createTime)));
        (co_await query(R"(INSERT INTO groupMembers (groupId, uid, joinTime) VALUES (?, ?, ?))",
                        parameters(group.groupId, group.masterUid, group.createTime)));
        (co_await query(R"(INSERT INTO conversationUserStates (conversationId, uid, joinedSeq) VALUES (?, ?, 1))",
                        parameters(group.conversationId, group.masterUid)));
        co_await _connection.command("COMMIT");
        result.error = ErrorCodes::Success;
        co_return result;
    } catch (const std::exception& error) {
        try {
            _connection.abort();
        } catch (...) {
        }
        result.diagnostic = error.what();
        co_return result;
    }
}
seastar::future<db::GroupOperationResult> mysql_store::execute(request_group_join operation)
{
    auto& group_id = operation.group_id;
    auto& uid = operation.uid;
    auto& message = operation.message;
    auto& now = operation.now;

    GroupOperationResult result;
    result.groupId = group_id;
    result.targetUid = uid;
    result.actorUid = uid;
    co_await _connection.command("START TRANSACTION");
    try {
        auto group = (co_await query(R"(SELECT masterUid, name FROM groupInfo WHERE groupId = ? FOR UPDATE)",
                                     parameters(group_id)))
                         .first_row();
        if (!group) {
            co_await _connection.command("ROLLBACK");
            result.error = ErrorCodes::GroupNotExists;
            co_return result;
        }
        result.masterUid = field<int64_t>(group[0]);
        result.groupName = field<std::string>(group[1]);
        if ((co_await query(R"(SELECT 1 FROM groupMembers WHERE groupId = ? AND uid = ?)", parameters(group_id, uid)))
                .first_row()) {
            co_await _connection.command("ROLLBACK");
            result.error = ErrorCodes::GroupAlreadyExists;
            co_return result;
        }
        auto insert = (co_await query(
            R"(INSERT IGNORE INTO groupApplys (groupId, requestorUid, message, createTime) VALUES (?, ?, ?, ?))",
            parameters(group_id, uid, message, now)));
        if (insert.affected_rows > 0) {
            result.recipientUids.push_back(result.masterUid);
        }
        co_await _connection.command("COMMIT");
        result.error = ErrorCodes::Success;
        co_return result;
    } catch (const std::exception& error) {
        try {
            _connection.abort();
        } catch (...) {
        }
        result.diagnostic = error.what();
        co_return result;
    }
}
seastar::future<db::GroupOperationResult> mysql_store::execute(reply_group_join operation)
{
    auto& group_id = operation.group_id;
    auto& actor_uid = operation.actor_uid;
    auto& requestor_uid = operation.requestor_uid;
    auto& accept = operation.accept;
    auto& now = operation.now;

    GroupOperationResult result;
    result.groupId = group_id;
    result.actorUid = actor_uid;
    result.targetUid = requestor_uid;
    co_await _connection.command("START TRANSACTION");
    try {
        auto group =
            (co_await query(R"(SELECT masterUid, conversationId, name FROM groupInfo WHERE groupId = ? FOR UPDATE)",
                            parameters(group_id)))
                .first_row();
        if (!group) {
            co_await _connection.command("ROLLBACK");
            result.error = ErrorCodes::GroupNotExists;
            co_return result;
        }
        result.masterUid = field<int64_t>(group[0]);
        result.conversationId = field<int64_t>(group[1]);
        result.groupName = field<std::string>(group[2]);
        if (result.masterUid != actor_uid) {
            co_await _connection.command("ROLLBACK");
            result.error = ErrorCodes::MessageOwnershipInvalid;
            co_return result;
        }
        if (!(co_await query(R"(SELECT 1 FROM groupApplys WHERE groupId = ? AND requestorUid = ? FOR UPDATE)",
                             parameters(group_id, requestor_uid)))
                 .first_row()) {
            co_await _connection.command("ROLLBACK");
            result.error = ErrorCodes::NotFound;
            co_return result;
        }
        (co_await query(R"(DELETE FROM groupApplys WHERE groupId = ? AND requestorUid = ?)",
                        parameters(group_id, requestor_uid)));
        if (accept) {
            (co_await query(R"(INSERT INTO groupMembers (groupId, uid, joinTime) VALUES (?, ?, ?))",
                            parameters(group_id, requestor_uid, now)));
            (co_await query(
                R"(INSERT INTO conversationUserStates (conversationId, uid, joinedSeq) SELECT conversationId, ?, latestSeq + 1 FROM conversations WHERE conversationId = ?)",
                parameters(requestor_uid, result.conversationId)));
            result.targetJoinTime = now;
            auto members = (co_await query(R"(SELECT uid FROM groupMembers WHERE groupId = ?)", parameters(group_id)));
            for (const auto& row : members.rows) {
                result.recipientUids.push_back(field<int64_t>(row[0]));
            }
        } else {
            result.recipientUids.push_back(requestor_uid);
        }
        co_await _connection.command("COMMIT");
        result.error = ErrorCodes::Success;
        co_return result;
    } catch (const std::exception& error) {
        try {
            _connection.abort();
        } catch (...) {
        }
        result.diagnostic = error.what();
        co_return result;
    }
}
seastar::future<db::GroupOperationResult> mysql_store::execute(remove_group_member operation)
{
    auto& group_id = operation.group_id;
    auto& actor_uid = operation.actor_uid;
    auto& target_uid = operation.target_uid;
    auto& dissolve_if_master = operation.dissolve_if_master;

    GroupOperationResult result;
    result.groupId = group_id;
    result.actorUid = actor_uid;
    result.targetUid = target_uid;
    co_await _connection.command("START TRANSACTION");
    try {
        auto group =
            (co_await query(R"(SELECT masterUid, conversationId, name FROM groupInfo WHERE groupId = ? FOR UPDATE)",
                            parameters(group_id)))
                .first_row();
        if (!group) {
            co_await _connection.command("ROLLBACK");
            result.error = ErrorCodes::GroupNotExists;
            co_return result;
        }
        result.masterUid = field<int64_t>(group[0]);
        result.conversationId = field<int64_t>(group[1]);
        result.groupName = field<std::string>(group[2]);
        if (actor_uid != result.masterUid && actor_uid != target_uid) {
            co_await _connection.command("ROLLBACK");
            result.error = ErrorCodes::MessageOwnershipInvalid;
            co_return result;
        }
        if (actor_uid != result.masterUid && target_uid != actor_uid) {
            co_await _connection.command("ROLLBACK");
            result.error = ErrorCodes::MessageOwnershipInvalid;
            co_return result;
        }
        if (!(co_await query(R"(SELECT 1 FROM groupMembers WHERE groupId = ? AND uid = ? FOR UPDATE)",
                             parameters(group_id, target_uid)))
                 .first_row()) {
            co_await _connection.command("ROLLBACK");
            result.error = ErrorCodes::NotFound;
            co_return result;
        }
        auto members = (co_await query(R"(SELECT uid FROM groupMembers WHERE groupId = ?)", parameters(group_id)));
        for (const auto& row : members.rows) {
            result.recipientUids.push_back(field<int64_t>(row[0]));
        }
        if (target_uid == result.masterUid && dissolve_if_master) {
            (co_await query(R"(DELETE FROM conversationUserStates WHERE conversationId = ?)",
                            parameters(result.conversationId)));
            (co_await query(R"(DELETE FROM groupApplys WHERE groupId = ?)", parameters(group_id)));
            (co_await query(R"(DELETE FROM groupMembers WHERE groupId = ?)", parameters(group_id)));
            (co_await query(R"(DELETE FROM groupInfo WHERE groupId = ?)", parameters(group_id)));
        } else {
            (co_await query(R"(DELETE FROM conversationUserStates WHERE conversationId = ? AND uid = ?)",
                            parameters(result.conversationId, target_uid)));
            (co_await query(R"(DELETE FROM groupMembers WHERE groupId = ? AND uid = ?)",
                            parameters(group_id, target_uid)));
        }
        co_await _connection.command("COMMIT");
        result.error = ErrorCodes::Success;
        co_return result;
    } catch (const std::exception& error) {
        try {
            _connection.abort();
        } catch (...) {
        }
        result.diagnostic = error.what();
        co_return result;
    }
}
seastar::future<db::GroupOperationResult> mysql_store::execute(pull_group operation)
{
    auto& group_id = operation.group_id;
    auto& uid = operation.uid;

    GroupOperationResult result;
    result.groupId = group_id;
    result.actorUid = uid;
    try {
        auto group = (co_await query(R"(SELECT masterUid, conversationId, name FROM groupInfo WHERE groupId = ?)",
                                     parameters(group_id)))
                         .first_row();
        if (!group) {
            result.error = ErrorCodes::GroupNotExists;
            co_return result;
        }
        result.masterUid = field<int64_t>(group[0]);
        result.conversationId = field<int64_t>(group[1]);
        result.groupName = field<std::string>(group[2]);
        if (!(co_await query(R"(SELECT 1 FROM groupMembers WHERE groupId = ? AND uid = ?)", parameters(group_id, uid)))
                 .first_row()) {
            result.error = ErrorCodes::MessageOwnershipInvalid;
            co_return result;
        }
        auto members = (co_await query(
            R"(SELECT gm.uid, COALESCE(ui.name, ''), gm.joinTime FROM groupMembers gm LEFT JOIN userInfo ui ON ui.uid = gm.uid WHERE gm.groupId = ? ORDER BY gm.joinTime, gm.uid)",
            parameters(group_id)));
        for (const auto& row : members.rows) {
            result.members.emplace_back(group_id, field<int64_t>(row[0]), field<std::string>(row[2]),
                                        field<std::string>(row[1]));
        }
        if (uid == result.masterUid) {
            auto applies = (co_await query(
                R"(SELECT requestorUid, message, createTime FROM groupApplys WHERE groupId = ? ORDER BY createTime, requestorUid)",
                parameters(group_id)));
            for (const auto& row : applies.rows) {
                result.applies.emplace_back(field<int64_t>(row[0]), group_id, field<std::string>(row[1]),
                                            field<std::string>(row[2]));
            }
        }
        result.error = ErrorCodes::Success;
        co_return result;
    } catch (const std::exception& error) {
        result.diagnostic = error.what();
        co_return result;
    }
}
seastar::future<int> mysql_store::execute(update_user_info_name operation)
{
    auto& uid = operation.uid;
    auto& name = operation.name;

    std::string f = R"(UPDATE userInfo SET name = ? WHERE uid = ?)";
    auto result = (co_await query(f, parameters(name, uid)));

    co_return 0;
}
seastar::future<int> mysql_store::execute(update_user_info_age operation)
{
    auto& uid = operation.uid;
    auto& age = operation.age;

    std::string f = R"(UPDATE userInfo SET age = ? WHERE uid = ?)";
    auto result = (co_await query(f, parameters(age, uid)));

    co_return 0;
}
seastar::future<int> mysql_store::execute(update_user_info_sex operation)
{
    auto& uid = operation.uid;
    auto& sex = operation.sex;

    std::string f = R"(UPDATE userInfo SET sex = ? WHERE uid = ?)";
    auto result = (co_await query(f, parameters(sex, uid)));

    co_return 0;
}
seastar::future<int> mysql_store::execute(update_user_info_head_image_url operation)
{
    auto& uid = operation.uid;
    auto& head_image_url = operation.head_image_url;

    std::string f = R"(UPDATE userInfo SET headImageURL = ? WHERE uid = ?)";
    auto result = (co_await query(f, parameters(head_image_url, uid)));

    co_return 0;
}
} // namespace imm::message_io
