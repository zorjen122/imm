#include "MessageService.h"

#include "Const.h"
#include "DbGlobal.h"
#include "Logger.h"
#include "Mysql.h"
#include <string>

namespace wimi
{
namespace
{

template <typename File> void FillFileInfo(protocol::FileInfo* output, const File& file)
{
    output->set_file_id(file.fileId);
    output->set_file_name(file.fileName);
    output->set_file_type(file.fileType);
    output->set_file_size(file.fileSize);
    output->set_sha256(file.sha256);
}

} // namespace

MessageService::AcceptedText MessageService::AcceptText(int64_t actorUid, const TcpPacket& request)
{
    AcceptedText result;
    const auto& send = request.send_text_request();
    auto* response = result.response.mutable_send_message_response();
    if (actorUid <= 0 || send.to() <= 0 || send.content().empty() || send.client_message_id().empty()) {
        LOG_WARN(logger, "[message/direct-text] 拒绝无效消息, from_uid: {}, to_uid: {}", actorUid, send.to());
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser,
                           "actor, to, content and client_message_id are required");
        return result;
    }

    const std::string sendDateTime = getCurrentDateTime();
    auto accepted = db::MysqlDao::GetInstance()->acceptDirectText(actorUid, send.to(), send.client_message_id(),
                                                                  send.content(), sendDateTime);
    SetTcpPacketStatus(result.response, accepted.error,
                       accepted.error == ErrorCodes::Success
                           ? std::string{}
                           : (accepted.diagnostic.empty() ? "persistent message accept failed" : accepted.diagnostic));
    if (accepted.error != ErrorCodes::Success) {
        if (!accepted.diagnostic.empty())
            LOG_ERROR(logger, "[message/direct-text] 消息保存失败, from_uid: {}, to_uid: {}, error: {}, detail: {}",
                      actorUid, send.to(), accepted.error, accepted.diagnostic);
        else
            LOG_WARN(logger, "[message/direct-text] 消息未被接受, from_uid: {}, to_uid: {}, error: {}", actorUid,
                     send.to(), accepted.error);
        return result;
    }

    response->set_message_id(accepted.messageId);
    response->set_conversation_id(accepted.conversationId);
    response->set_conversation_seq(accepted.conversationSeq);

    auto* push = result.forwardPacket.mutable_text_message_push();
    push->set_message_id(accepted.messageId);
    push->set_from(actorUid);
    push->set_to(send.to());
    push->set_content(send.content());
    push->set_client_message_id(send.client_message_id());
    push->set_conversation_id(accepted.conversationId);
    push->set_conversation_seq(accepted.conversationSeq);
    push->set_send_date_time(sendDateTime);
    result.recipientUid = send.to();
    result.shouldForward = !accepted.duplicate;
    LOG_INFO(logger,
             "[message/direct-text] 消息已保存, message_id: {}, conversation_id: {}, conversation_seq: {}, "
             "from_uid: {}, to_uid: {}, duplicate: {}",
             accepted.messageId, accepted.conversationId, accepted.conversationSeq, actorUid, send.to(),
             accepted.duplicate);
    return result;
}

MessageService::AcceptedGroupText MessageService::AcceptGroupText(int64_t actorUid, const TcpPacket& request)
{
    AcceptedGroupText result;
    const auto& send = request.send_group_text_request();
    auto* response = result.response.mutable_send_message_response();
    if (actorUid <= 0 || send.group_id() <= 0 || send.content().empty() || send.client_message_id().empty()) {
        LOG_WARN(logger, "[message/group-text] 拒绝无效消息, from_uid: {}, group_id: {}", actorUid, send.group_id());
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser,
                           "actor, group_id, content and client_message_id are required");
        return result;
    }

    const std::string sendDateTime = getCurrentDateTime();
    auto accepted = db::MysqlDao::GetInstance()->acceptGroupText(actorUid, send.group_id(), send.client_message_id(),
                                                                 send.content(), sendDateTime);
    SetTcpPacketStatus(result.response, accepted.error,
                       accepted.error == ErrorCodes::Success
                           ? std::string{}
                           : (accepted.diagnostic.empty() ? getErrorCodeMsg(accepted.error) : accepted.diagnostic));
    if (accepted.error != ErrorCodes::Success) {
        if (!accepted.diagnostic.empty())
            LOG_ERROR(logger, "[message/group-text] 消息保存失败, from_uid: {}, group_id: {}, error: {}, detail: {}",
                      actorUid, send.group_id(), accepted.error, accepted.diagnostic);
        else
            LOG_WARN(logger, "[message/group-text] 消息未被接受, from_uid: {}, group_id: {}, error: {}", actorUid,
                     send.group_id(), accepted.error);
        return result;
    }

    response->set_message_id(accepted.messageId);
    response->set_conversation_id(accepted.conversationId);
    response->set_conversation_seq(accepted.conversationSeq);
    auto* push = result.forwardPacket.mutable_text_message_push();
    push->set_message_id(accepted.messageId);
    push->set_from(actorUid);
    push->set_group_id(send.group_id());
    push->set_content(send.content());
    push->set_client_message_id(send.client_message_id());
    push->set_conversation_id(accepted.conversationId);
    push->set_conversation_seq(accepted.conversationSeq);
    push->set_send_date_time(sendDateTime);
    result.recipientUids = std::move(accepted.recipientUids);
    result.shouldForward = !accepted.duplicate;
    LOG_INFO(logger,
             "[message/group-text] 消息已保存, message_id: {}, conversation_id: {}, conversation_seq: {}, "
             "from_uid: {}, group_id: {}, recipients: {}, duplicate: {}",
             accepted.messageId, accepted.conversationId, accepted.conversationSeq, actorUid, send.group_id(),
             result.recipientUids.size(), accepted.duplicate);
    return result;
}

MessageService::AckResult MessageService::Ack(int64_t actorUid, const TcpPacket& request, const std::string& deviceId)
{
    AckResult result;
    const auto& receipt = request.receipt();
    if (receipt.seq() <= 0 || receipt.conversation_id() <= 0 || receipt.conversation_seq() <= 0 ||
        (receipt.receipt_type() != protocol::RECEIPT_TYPE_DELIVERED &&
         receipt.receipt_type() != protocol::RECEIPT_TYPE_READ)) {
        LOG_WARN(logger,
                 "[message/receipt] 拒绝无效回执, uid: {}, message_id: {}, conversation_id: {}, "
                 "conversation_seq: {}",
                 actorUid, receipt.seq(), receipt.conversation_id(), receipt.conversation_seq());
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser);
        return result;
    }

    const short messageStatus = receipt.receipt_type() == protocol::RECEIPT_TYPE_READ ? db::Message::Status::READ
                                                                                      : db::Message::Status::DELIVERED;
    const std::string readTime =
        receipt.receipt_type() == protocol::RECEIPT_TYPE_READ ? getCurrentDateTime() : std::string{};
    const auto acknowledged = db::MysqlDao::GetInstance()->acknowledgeConversationMessage(
        receipt.seq(), actorUid, deviceId, receipt.conversation_id(), receipt.conversation_seq(), messageStatus,
        readTime);
    if (acknowledged.updated <= 0) {
        LOG_WARN(logger, "[message/receipt] 回执更新失败或消息不属于当前用户, uid: {}, message_id: {}, result: {}",
                 actorUid, receipt.seq(), acknowledged.updated);
        SetTcpPacketStatus(result.response,
                           acknowledged.updated == -1 ? ErrorCodes::MysqlFailed : ErrorCodes::MessageOwnershipInvalid);
        return result;
    }

    SetTcpPacketStatus(result.response, ErrorCodes::Success);
    if (receipt.receipt_type() == protocol::RECEIPT_TYPE_READ && acknowledged.directConversation &&
        acknowledged.readStateChanged) {
        result.senderUid = acknowledged.senderUid;
        result.shouldForwardRead = result.senderUid > 0;
        auto* push = result.readReceipt.mutable_read_receipt_push();
        push->set_message_id(receipt.seq());
        push->set_from(actorUid);
        push->set_to(result.senderUid);
        push->set_conversation_id(receipt.conversation_id());
        push->set_conversation_seq(receipt.conversation_seq());
    }
    LOG_DEBUG(logger, "[message/receipt] 回执已更新, uid: {}, message_id: {}, receipt_type: {}, forward_read: {}",
              actorUid, receipt.seq(), static_cast<int>(receipt.receipt_type()), result.shouldForwardRead);
    return result;
}

MessageService::AcceptedFile MessageService::AcceptFile(int64_t actorUid, const TcpPacket& request)
{
    AcceptedFile result;
    const auto& send = request.send_file_request();
    auto* response = result.response.mutable_send_message_response();
    if (actorUid <= 0 || send.to() <= 0 || !send.has_file() || send.client_message_id().empty()) {
        LOG_WARN(logger, "[message/direct-file] 拒绝无效消息, from_uid: {}, to_uid: {}", actorUid, send.to());
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser, "to, file and client_message_id are required");
        return result;
    }
    const auto file = db::MysqlDao::GetInstance()->findReadyFile(send.file().file_id(), actorUid);
    if (!file) {
        LOG_WARN(logger, "[message/direct-file] 文件未就绪, from_uid: {}, to_uid: {}, file_id: {}", actorUid, send.to(),
                 send.file().file_id());
        SetTcpPacketStatus(result.response, ErrorCodes::NotFound, "ready file not found");
        return result;
    }
    const auto accepted = db::MysqlDao::GetInstance()->acceptDirectText(
        actorUid, send.to(), send.client_message_id(), file->fileId, getCurrentDateTime(), db::Message::Type::FILE);
    SetTcpPacketStatus(result.response, accepted.error, accepted.diagnostic);
    if (accepted.error != ErrorCodes::Success) {
        LOG_ERROR(logger, "[message/direct-file] 文件消息保存失败, from_uid: {}, to_uid: {}, error: {}", actorUid,
                  send.to(), accepted.error);
        return result;
    }

    response->set_message_id(accepted.messageId);
    response->set_conversation_id(accepted.conversationId);
    response->set_conversation_seq(accepted.conversationSeq);
    auto* push = result.forwardPacket.mutable_file_message_push();
    push->set_message_id(accepted.messageId);
    push->set_from(actorUid);
    push->set_to(send.to());
    FillFileInfo(push->mutable_file(), *file);
    push->set_client_message_id(send.client_message_id());
    push->set_conversation_id(accepted.conversationId);
    push->set_conversation_seq(accepted.conversationSeq);
    result.recipientUid = send.to();
    result.shouldForward = !accepted.duplicate;
    LOG_INFO(logger,
             "[message/direct-file] 文件消息已保存, message_id: {}, conversation_id: {}, from_uid: {}, "
             "to_uid: {}, file_id: {}, duplicate: {}",
             accepted.messageId, accepted.conversationId, actorUid, send.to(), file->fileId, accepted.duplicate);
    return result;
}

MessageService::AcceptedGroupText MessageService::AcceptGroupFile(int64_t actorUid, const TcpPacket& request)
{
    AcceptedGroupText result;
    const auto& send = request.send_group_file_request();
    auto* response = result.response.mutable_send_message_response();
    if (actorUid <= 0 || send.group_id() <= 0 || !send.has_file() || send.client_message_id().empty()) {
        LOG_WARN(logger, "[message/group-file] 拒绝无效消息, from_uid: {}, group_id: {}", actorUid, send.group_id());
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser,
                           "group_id, file and client_message_id are required");
        return result;
    }
    const auto file = db::MysqlDao::GetInstance()->findReadyFile(send.file().file_id(), actorUid);
    if (!file) {
        LOG_WARN(logger, "[message/group-file] 文件未就绪, from_uid: {}, group_id: {}, file_id: {}", actorUid,
                 send.group_id(), send.file().file_id());
        SetTcpPacketStatus(result.response, ErrorCodes::NotFound, "ready file not found");
        return result;
    }
    const auto accepted = db::MysqlDao::GetInstance()->acceptGroupMessage(
        actorUid, send.group_id(), send.client_message_id(), file->fileId, db::Message::Type::FILE,
        getCurrentDateTime(), true);
    SetTcpPacketStatus(result.response, accepted.error, accepted.diagnostic);
    if (accepted.error != ErrorCodes::Success) {
        LOG_ERROR(logger, "[message/group-file] 文件消息保存失败, from_uid: {}, group_id: {}, error: {}", actorUid,
                  send.group_id(), accepted.error);
        return result;
    }

    response->set_message_id(accepted.messageId);
    response->set_conversation_id(accepted.conversationId);
    response->set_conversation_seq(accepted.conversationSeq);
    auto* push = result.forwardPacket.mutable_file_message_push();
    push->set_message_id(accepted.messageId);
    push->set_from(actorUid);
    push->set_group_id(send.group_id());
    FillFileInfo(push->mutable_file(), *file);
    push->set_client_message_id(send.client_message_id());
    push->set_conversation_id(accepted.conversationId);
    push->set_conversation_seq(accepted.conversationSeq);
    result.recipientUids = accepted.recipientUids;
    result.shouldForward = !accepted.duplicate;
    LOG_INFO(logger,
             "[message/group-file] 文件消息已保存, message_id: {}, conversation_id: {}, from_uid: {}, "
             "group_id: {}, file_id: {}, recipients: {}, duplicate: {}",
             accepted.messageId, accepted.conversationId, actorUid, send.group_id(), file->fileId,
             result.recipientUids.size(), accepted.duplicate);
    return result;
}

TcpPacket MessageService::PullConversationMessages(int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& pull = request.pull_conversation_messages_request();
    if (pull.conversation_id() <= 0) {
        LOG_WARN(logger, "[message/history] 拒绝无效拉取请求, uid: {}, conversation_id: {}", actorUid,
                 pull.conversation_id());
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        return rsp;
    }

    const int limit = pull.limit() > 0 ? pull.limit() : 50;
    auto sync =
        db::MysqlDao::GetInstance()->syncConversation(actorUid, pull.conversation_id(), pull.after_seq(), limit);
    auto* response = rsp.mutable_pull_conversation_messages_response();
    response->set_conversation_id(pull.conversation_id());
    response->set_latest_seq(sync.latestSeq);
    response->set_next_seq(sync.nextSeq);
    response->set_has_more(sync.hasMore);
    if (sync.conversationType == 1)
        response->set_conversation_type(protocol::CONVERSATION_TYPE_DIRECT);
    else if (sync.conversationType == 2)
        response->set_conversation_type(protocol::CONVERSATION_TYPE_GROUP);
    SetTcpPacketStatus(rsp, sync.error);
    if (sync.error != ErrorCodes::Success) {
        LOG_WARN(logger, "[message/history] 消息记录读取失败, uid: {}, conversation_id: {}, error: {}", actorUid,
                 pull.conversation_id(), sync.error);
        return rsp;
    }
    for (const auto& message : sync.messages) {
        auto* item = response->add_messages();
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
            FillFileInfo(file, *message.file);
            file->set_expired(message.file->expired);
        }
    }
    LOG_DEBUG(logger,
              "[message/history] 消息记录读取成功, uid: {}, conversation_id: {}, messages: {}, "
              "next_seq: {}, has_more: {}",
              actorUid, pull.conversation_id(), sync.messages.size(), sync.nextSeq, sync.hasMore);
    return rsp;
}

} // namespace wimi
