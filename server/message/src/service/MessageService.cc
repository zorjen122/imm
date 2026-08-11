#include "MessageService.h"

#include "Const.h"
#include "DbGlobal.h"
#include "Logger.h"
#include "Mysql.h"
#include "Redis.h"
#include "RequestContext.h"

#include <string>

namespace wimi
{

MessageService::AcceptedText MessageService::AcceptText(TcpPacket request)
{
    AcceptedText result;
    const int64_t from = request.uid();
    const int64_t to = request.to();
    const std::string data = request.data();
    const std::string clientMessageId = request.client_message_id();

    result.response.set_client_message_id(clientMessageId);
    if (from <= 0 || to <= 0 || data.empty() || !request.has_client_message_id() || clientMessageId.empty()) {
        result.response.set_error(ErrorCodes::JsonParser);
        result.response.set_message("actor, to, data and client_message_id are required");
        result.response.set_retryable(false);
        return result;
    }

    const std::string sendDateTime = getCurrentDateTime();
    auto accepted = db::MysqlDao::GetInstance()->acceptDirectText(from, to, clientMessageId, data, sendDateTime);
    result.response.set_error(accepted.error);
    result.response.set_retryable(isRetryableError(accepted.error));
    if (accepted.error != ErrorCodes::Success) {
        if (!accepted.diagnostic.empty())
            LOG_ERROR(dbLogger, "direct message accept failed: {}", accepted.diagnostic);
        result.response.set_message(accepted.diagnostic.empty() ? "persistent message accept failed"
                                                                : accepted.diagnostic);
        return result;
    }

    result.response.set_message_id(accepted.messageId);
    result.response.set_conversation_id(accepted.conversationId);
    result.response.set_conversation_seq(accepted.conversationSeq);
    result.response.set_message_state(protocol::MESSAGE_STATE_ACCEPTED);
    result.response.set_retryable(false);

    request.set_uid(from);
    request.set_from(from);
    request.set_seq(accepted.messageId);
    request.set_message_id(accepted.messageId);
    request.set_client_message_id(clientMessageId);
    request.set_conversation_id(accepted.conversationId);
    request.set_conversation_seq(accepted.conversationSeq);
    request.set_message_state(protocol::MESSAGE_STATE_ACCEPTED);
    request.set_send_date_time(sendDateTime);
    result.forwardPacket = std::move(request);
    result.recipientUid = to;
    result.shouldForward = !accepted.duplicate;
    return result;
}

MessageService::AcceptedGroupText MessageService::AcceptGroupText(TcpPacket request)
{
    AcceptedGroupText result;
    const int64_t sender = request.uid();
    const int64_t groupId = request.group_id();
    const std::string content = request.data();
    const std::string clientMessageId = request.client_message_id();

    result.response.set_client_message_id(clientMessageId);
    result.response.set_group_id(groupId);
    if (sender <= 0 || groupId <= 0 || content.empty() || !request.has_client_message_id() || clientMessageId.empty()) {
        result.response.set_error(ErrorCodes::JsonParser);
        result.response.set_message("actor, group_id, data and client_message_id are required");
        result.response.set_retryable(false);
        return result;
    }

    const std::string sendDateTime = getCurrentDateTime();
    auto accepted =
        db::MysqlDao::GetInstance()->acceptGroupText(sender, groupId, clientMessageId, content, sendDateTime);
    result.response.set_error(accepted.error);
    result.response.set_retryable(isRetryableError(accepted.error));
    if (accepted.error != ErrorCodes::Success) {
        if (!accepted.diagnostic.empty())
            LOG_ERROR(dbLogger, "group message accept failed: {}", accepted.diagnostic);
        result.response.set_message(accepted.diagnostic.empty() ? getErrorCodeMsg(accepted.error)
                                                                : accepted.diagnostic);
        return result;
    }

    result.response.set_message_id(accepted.messageId);
    result.response.set_conversation_id(accepted.conversationId);
    result.response.set_conversation_seq(accepted.conversationSeq);
    result.response.set_message_state(protocol::MESSAGE_STATE_ACCEPTED);
    result.response.set_retryable(false);

    request.set_from(sender);
    request.set_group_id(groupId);
    request.set_seq(accepted.messageId);
    request.set_message_id(accepted.messageId);
    request.set_client_message_id(clientMessageId);
    request.set_conversation_id(accepted.conversationId);
    request.set_conversation_seq(accepted.conversationSeq);
    request.set_message_state(protocol::MESSAGE_STATE_ACCEPTED);
    request.set_send_date_time(sendDateTime);
    result.forwardPacket = std::move(request);
    result.recipientUids = std::move(accepted.recipientUids);
    result.shouldForward = !accepted.duplicate;
    return result;
}

MessageService::AckResult MessageService::Ack(TcpPacket& request, const std::string& deviceId)
{
    AckResult result;
    auto& rsp = result.response;
    int64_t seq = request.seq();
    int64_t uid = request.uid();
    if (seq <= 0) {
        rsp.set_error(ErrorCodes::JsonParser);
        return result;
    }
    if (!request.has_receipt_type() || !request.has_conversation_id() || !request.has_conversation_seq() ||
        request.conversation_id() <= 0 || request.conversation_seq() <= 0) {
        rsp.set_error(ErrorCodes::JsonParser);
        return result;
    }
    const auto receiptType = request.receipt_type();
    if (receiptType != protocol::RECEIPT_TYPE_DELIVERED && receiptType != protocol::RECEIPT_TYPE_READ) {
        rsp.set_error(ErrorCodes::JsonParser);
        return result;
    }
    short status =
        receiptType == protocol::RECEIPT_TYPE_READ ? db::Message::Status::READ : db::Message::Status::DELIVERED;
    std::string readTime = receiptType == protocol::RECEIPT_TYPE_READ ? getCurrentDateTime() : std::string{};

    const auto acknowledged = db::MysqlDao::GetInstance()->acknowledgeConversationMessage(
        seq, uid, deviceId, request.conversation_id(), request.conversation_seq(), status, readTime);
    if (acknowledged.updated <= 0) {
        LOG_WARN(wimi::businessLogger, "ACK所有权校验失败或消息不存在, seq: {}, principal: {}", seq, uid);
        rsp.set_error(acknowledged.updated == -1 ? ErrorCodes::MysqlFailed : ErrorCodes::MessageOwnershipInvalid);
        return result;
    }

    rsp.set_error(ErrorCodes::Success);
    rsp.set_message_id(seq);
    rsp.set_message_state(receiptType == protocol::RECEIPT_TYPE_READ ? protocol::MESSAGE_STATE_READ
                                                                     : protocol::MESSAGE_STATE_DELIVERED);
    if (receiptType == protocol::RECEIPT_TYPE_READ && acknowledged.directConversation &&
        acknowledged.readStateChanged) {
        result.senderUid = acknowledged.senderUid;
        result.shouldForwardRead = result.senderUid > 0;
        result.readReceipt.set_seq(seq);
        result.readReceipt.set_message_id(seq);
        result.readReceipt.set_from(uid);
        result.readReceipt.set_to(result.senderUid);
        result.readReceipt.set_conversation_id(request.conversation_id());
        result.readReceipt.set_conversation_seq(request.conversation_seq());
        result.readReceipt.set_message_state(protocol::MESSAGE_STATE_READ);
    }
    return result;
}

TcpPacket MessageService::SendFile(uint32_t msgID, TcpPacket& request)
{
    (void)msgID;
    return AcceptFile(request).response;
}

MessageService::AcceptedFile MessageService::AcceptFile(TcpPacket request)
{
    AcceptedFile result;
    const int64_t sender = request.uid();
    const int64_t recipient = request.to();
    if (sender <= 0 || recipient <= 0 || !request.has_file() || !request.has_client_message_id() ||
        request.client_message_id().empty()) {
        result.response.set_error(ErrorCodes::JsonParser);
        result.response.set_message("to, file and client_message_id are required");
        return result;
    }
    const auto file = db::MysqlDao::GetInstance()->findReadyFile(request.file().file_id(), sender);
    if (!file) {
        result.response.set_error(ErrorCodes::NotFound);
        result.response.set_message("ready file not found");
        return result;
    }
    const auto accepted = db::MysqlDao::GetInstance()->acceptDirectText(
        sender, recipient, request.client_message_id(), file->fileId, getCurrentDateTime(), db::Message::Type::FILE);
    result.response.set_error(accepted.error);
    result.response.set_retryable(isRetryableError(accepted.error));
    result.response.set_client_message_id(request.client_message_id());
    if (accepted.error != ErrorCodes::Success) {
        result.response.set_message(accepted.diagnostic);
        return result;
    }
    result.response.set_message_id(accepted.messageId);
    result.response.set_conversation_id(accepted.conversationId);
    result.response.set_conversation_seq(accepted.conversationSeq);
    result.response.set_message_state(protocol::MESSAGE_STATE_ACCEPTED);
    request.set_from(sender);
    request.set_seq(accepted.messageId);
    request.set_message_id(accepted.messageId);
    request.set_conversation_id(accepted.conversationId);
    request.set_conversation_seq(accepted.conversationSeq);
    request.set_type(db::Message::Type::FILE);
    request.set_content(file->fileId);
    auto* metadata = request.mutable_file();
    metadata->set_file_id(file->fileId);
    metadata->set_file_name(file->fileName);
    metadata->set_file_type(file->fileType);
    metadata->set_file_size(file->fileSize);
    metadata->set_sha256(file->sha256);
    result.forwardPacket = std::move(request);
    result.recipientUid = recipient;
    result.shouldForward = !accepted.duplicate;
    return result;
}

MessageService::AcceptedGroupText MessageService::AcceptGroupFile(TcpPacket request)
{
    AcceptedGroupText result;
    const int64_t sender = request.uid();
    const int64_t groupId = request.group_id();
    result.response.set_group_id(groupId);
    result.response.set_client_message_id(request.client_message_id());
    if (sender <= 0 || groupId <= 0 || !request.has_file() || !request.has_client_message_id() ||
        request.client_message_id().empty()) {
        result.response.set_error(ErrorCodes::JsonParser);
        result.response.set_message("group_id, file and client_message_id are required");
        return result;
    }
    const auto file = db::MysqlDao::GetInstance()->findReadyFile(request.file().file_id(), sender);
    if (!file) {
        result.response.set_error(ErrorCodes::NotFound);
        result.response.set_message("ready file not found");
        return result;
    }
    const auto accepted =
        db::MysqlDao::GetInstance()->acceptGroupMessage(sender, groupId, request.client_message_id(), file->fileId,
                                                        db::Message::Type::FILE, getCurrentDateTime(), true);
    result.response.set_error(accepted.error);
    result.response.set_retryable(isRetryableError(accepted.error));
    if (accepted.error != ErrorCodes::Success) {
        result.response.set_message(accepted.diagnostic);
        return result;
    }
    result.response.set_message_id(accepted.messageId);
    result.response.set_conversation_id(accepted.conversationId);
    result.response.set_conversation_seq(accepted.conversationSeq);
    result.response.set_message_state(protocol::MESSAGE_STATE_ACCEPTED);
    request.set_from(sender);
    request.set_type(db::Message::Type::FILE);
    request.set_content(file->fileId);
    request.set_seq(accepted.messageId);
    request.set_message_id(accepted.messageId);
    request.set_conversation_id(accepted.conversationId);
    request.set_conversation_seq(accepted.conversationSeq);
    auto* metadata = request.mutable_file();
    metadata->set_file_id(file->fileId);
    metadata->set_file_name(file->fileName);
    metadata->set_file_type(file->fileType);
    metadata->set_file_size(file->fileSize);
    metadata->set_sha256(file->sha256);
    result.forwardPacket = std::move(request);
    result.recipientUids = accepted.recipientUids;
    result.shouldForward = !accepted.duplicate;
    return result;
}

TcpPacket MessageService::SendGroupText(uint32_t msgID, TcpPacket& request)
{
    /*
      1.检查群聊
      2.建立<seq, [member1, member2, ...]>映射
      3.按成员在线状态分发消息
    */
    return {};
}

TcpPacket MessageService::SendGroupFile(uint32_t, TcpPacket& request) { return AcceptGroupFile(request).response; }

TcpPacket MessageService::SendText(uint32_t msgID, TcpPacket& request)
{
    (void)msgID;
    auto accepted = AcceptText(request);
    return accepted.response;
}

TcpPacket MessageService::PullConversationMessages(uint32_t msgID, TcpPacket& request)
{
    TcpPacket rsp;

    if (!request.has_conversation_id() || request.conversation_id() <= 0) {
        rsp.set_error(ErrorCodes::JsonParser);
        return rsp;
    }

    const int64_t conversationId = request.conversation_id();
    const int64_t afterSeq = request.has_after_seq() ? request.after_seq() : 0;
    const int limit = request.has_limit() ? request.limit() : 50;
    auto sync = db::MysqlDao::GetInstance()->syncConversation(request.uid(), conversationId, afterSeq, limit);
    rsp.set_error(sync.error);
    rsp.set_conversation_id(conversationId);
    rsp.set_latest_seq(sync.latestSeq);
    rsp.set_next_seq(sync.nextSeq);
    rsp.set_has_more(sync.hasMore);
    if (sync.conversationType == 1)
        rsp.set_conversation_type(protocol::CONVERSATION_TYPE_DIRECT);
    else if (sync.conversationType == 2)
        rsp.set_conversation_type(protocol::CONVERSATION_TYPE_GROUP);
    if (sync.error != ErrorCodes::Success)
        return rsp;
    for (const auto& message : sync.messages) {
        auto* item = rsp.add_message_list();
        item->set_message_id(message.messageId);
        item->set_from(message.senderId);
        item->set_to(message.receiverId);
        item->set_conversation_id(message.conversationId);
        item->set_conversation_seq(message.conversationSeq);
        item->set_client_message_id(message.clientMessageId);
        item->set_type(message.type);
        item->set_content(message.content);
        item->set_status(message.status);
        item->set_send_date_time(message.sendDateTime);
        item->set_read_date_time(message.readDateTime);
        if (message.file) {
            auto* file = item->mutable_file();
            file->set_file_id(message.file->fileId);
            file->set_file_name(message.file->fileName);
            file->set_file_type(message.file->fileType);
            file->set_file_size(message.file->fileSize);
            file->set_sha256(message.file->sha256);
            file->set_expired(message.file->expired);
        }
    }
    return rsp;
}

} // namespace wimi
