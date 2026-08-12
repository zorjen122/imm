#include "MessageService.h"

#include "Const.h"
#include "DbGlobal.h"
#include "Logger.h"
#include "Mysql.h"
#include "RequestContext.h"

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
            LOG_ERROR(dbLogger, "direct message accept failed: {}", accepted.diagnostic);
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
    return result;
}

MessageService::AcceptedGroupText MessageService::AcceptGroupText(int64_t actorUid, const TcpPacket& request)
{
    AcceptedGroupText result;
    const auto& send = request.send_group_text_request();
    auto* response = result.response.mutable_send_message_response();
    if (actorUid <= 0 || send.group_id() <= 0 || send.content().empty() || send.client_message_id().empty()) {
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
            LOG_ERROR(dbLogger, "group message accept failed: {}", accepted.diagnostic);
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
    return result;
}

MessageService::AckResult MessageService::Ack(int64_t actorUid, const TcpPacket& request, const std::string& deviceId)
{
    AckResult result;
    const auto& receipt = request.receipt();
    if (receipt.seq() <= 0 || receipt.conversation_id() <= 0 || receipt.conversation_seq() <= 0 ||
        (receipt.receipt_type() != protocol::RECEIPT_TYPE_DELIVERED &&
         receipt.receipt_type() != protocol::RECEIPT_TYPE_READ)) {
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
        LOG_WARN(wimi::businessLogger, "ACK所有权校验失败或消息不存在, seq: {}, principal: {}", receipt.seq(),
                 actorUid);
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
    return result;
}

MessageService::AcceptedFile MessageService::AcceptFile(int64_t actorUid, const TcpPacket& request)
{
    AcceptedFile result;
    const auto& send = request.send_file_request();
    auto* response = result.response.mutable_send_message_response();
    if (actorUid <= 0 || send.to() <= 0 || !send.has_file() || send.client_message_id().empty()) {
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser, "to, file and client_message_id are required");
        return result;
    }
    const auto file = db::MysqlDao::GetInstance()->findReadyFile(send.file().file_id(), actorUid);
    if (!file) {
        SetTcpPacketStatus(result.response, ErrorCodes::NotFound, "ready file not found");
        return result;
    }
    const auto accepted = db::MysqlDao::GetInstance()->acceptDirectText(
        actorUid, send.to(), send.client_message_id(), file->fileId, getCurrentDateTime(), db::Message::Type::FILE);
    SetTcpPacketStatus(result.response, accepted.error, accepted.diagnostic);
    if (accepted.error != ErrorCodes::Success)
        return result;

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
    return result;
}

MessageService::AcceptedGroupText MessageService::AcceptGroupFile(int64_t actorUid, const TcpPacket& request)
{
    AcceptedGroupText result;
    const auto& send = request.send_group_file_request();
    auto* response = result.response.mutable_send_message_response();
    if (actorUid <= 0 || send.group_id() <= 0 || !send.has_file() || send.client_message_id().empty()) {
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser,
                           "group_id, file and client_message_id are required");
        return result;
    }
    const auto file = db::MysqlDao::GetInstance()->findReadyFile(send.file().file_id(), actorUid);
    if (!file) {
        SetTcpPacketStatus(result.response, ErrorCodes::NotFound, "ready file not found");
        return result;
    }
    const auto accepted = db::MysqlDao::GetInstance()->acceptGroupMessage(
        actorUid, send.group_id(), send.client_message_id(), file->fileId, db::Message::Type::FILE,
        getCurrentDateTime(), true);
    SetTcpPacketStatus(result.response, accepted.error, accepted.diagnostic);
    if (accepted.error != ErrorCodes::Success)
        return result;

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
    return result;
}

TcpPacket MessageService::SendText(uint32_t, int64_t actorUid, TcpPacket& request)
{
    return AcceptText(actorUid, request).response;
}

TcpPacket MessageService::SendGroupText(uint32_t, int64_t actorUid, TcpPacket& request)
{
    return AcceptGroupText(actorUid, request).response;
}

TcpPacket MessageService::SendFile(uint32_t, int64_t actorUid, TcpPacket& request)
{
    return AcceptFile(actorUid, request).response;
}

TcpPacket MessageService::SendGroupFile(uint32_t, int64_t actorUid, TcpPacket& request)
{
    return AcceptGroupFile(actorUid, request).response;
}

TcpPacket MessageService::PullConversationMessages(uint32_t, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& pull = request.pull_conversation_messages_request();
    if (pull.conversation_id() <= 0) {
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
    if (sync.error != ErrorCodes::Success)
        return rsp;
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
    return rsp;
}

} // namespace wimi
