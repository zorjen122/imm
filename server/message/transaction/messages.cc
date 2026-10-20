#include "transaction/messages.hh"

#include "Const.h"
#include "DbGlobal.h"
#include "Logger.h"

#include <string>

namespace imm::message_transaction
{
namespace
{

template <typename File>
    requires requires(const File& value) {
        value.fileId;
        value.fileName;
        value.fileType;
        value.fileSize;
        value.sha256;
    }
void fill_file_info(protocol::FileInfo* output, const File& file)
{
    output->set_file_id(file.fileId);
    output->set_file_name(file.fileName);
    output->set_file_type(file.fileType);
    output->set_file_size(file.fileSize);
    output->set_sha256(file.sha256);
}

} // namespace

seastar::future<accepted_message> messages::accept_text(int64_t actor_uid, const protocol::Packet& request)
{
    accepted_message result;
    const auto& send = request.send_text_request();
    auto* response = result.response.mutable_send_message_response();
    if (actor_uid <= 0 || send.to() <= 0 || send.content().empty() || send.client_message_id().empty()) {
        LOG_WARN(logger, "[message/direct-text] 拒绝无效消息, from_uid: {}, to_uid: {}", actor_uid, send.to());
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser,
                           "actor, to, content and client_message_id are required");
        co_return result;
    }

    const std::string send_date_time = getCurrentDateTime();
    message_io::accept_direct_text operation_accept_direct_text{actor_uid, send.to(), send.client_message_id(),
                                                                send.content(), send_date_time};
    auto accepted = co_await _io.submit(std::move(operation_accept_direct_text));
    SetTcpPacketStatus(result.response, accepted.error,
                       accepted.error == ErrorCodes::Success
                           ? std::string{}
                           : (accepted.diagnostic.empty() ? "persistent message accept failed" : accepted.diagnostic));
    if (accepted.error != ErrorCodes::Success) {
        if (!accepted.diagnostic.empty()) {
            LOG_ERROR(logger, "[message/direct-text] 消息保存失败, from_uid: {}, to_uid: {}, error: {}, detail: {}",
                      actor_uid, send.to(), accepted.error, accepted.diagnostic);
        } else {
            LOG_WARN(logger, "[message/direct-text] 消息未被接受, from_uid: {}, to_uid: {}, error: {}", actor_uid,
                     send.to(), accepted.error);
        }
        co_return result;
    }

    response->set_message_id(accepted.messageId);
    response->set_conversation_id(accepted.conversationId);
    response->set_conversation_seq(accepted.conversationSeq);

    auto* push = result.forward_packet.mutable_text_message_push();
    push->set_message_id(accepted.messageId);
    push->set_from(actor_uid);
    push->set_to(send.to());
    push->set_content(send.content());
    push->set_client_message_id(send.client_message_id());
    push->set_conversation_id(accepted.conversationId);
    push->set_conversation_seq(accepted.conversationSeq);
    push->set_send_date_time(send_date_time);
    result.recipients.push_back(send.to());
    result.should_forward = !accepted.duplicate;
    LOG_INFO(logger,
             "[message/direct-text] 消息已保存, message_id: {}, conversation_id: {}, conversation_seq: {}, "
             "from_uid: {}, to_uid: {}, duplicate: {}",
             accepted.messageId, accepted.conversationId, accepted.conversationSeq, actor_uid, send.to(),
             accepted.duplicate);
    co_return result;
}

seastar::future<accepted_message> messages::accept_group_text(int64_t actor_uid, const protocol::Packet& request)
{
    accepted_message result;
    const auto& send = request.send_group_text_request();
    auto* response = result.response.mutable_send_message_response();
    if (actor_uid <= 0 || send.group_id() <= 0 || send.content().empty() || send.client_message_id().empty()) {
        LOG_WARN(logger, "[message/group-text] 拒绝无效消息, from_uid: {}, group_id: {}", actor_uid, send.group_id());
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser,
                           "actor, group_id, content and client_message_id are required");
        co_return result;
    }

    const std::string send_date_time = getCurrentDateTime();
    message_io::accept_group_message operation_accept_group_message{
        actor_uid, send.group_id(), send.client_message_id(), send.content(), db::Message::TEXT, send_date_time};
    auto accepted = co_await _io.submit(std::move(operation_accept_group_message));
    SetTcpPacketStatus(result.response, accepted.error,
                       accepted.error == ErrorCodes::Success
                           ? std::string{}
                           : (accepted.diagnostic.empty() ? getErrorCodeMsg(accepted.error) : accepted.diagnostic));
    if (accepted.error != ErrorCodes::Success) {
        if (!accepted.diagnostic.empty()) {
            LOG_ERROR(logger, "[message/group-text] 消息保存失败, from_uid: {}, group_id: {}, error: {}, detail: {}",
                      actor_uid, send.group_id(), accepted.error, accepted.diagnostic);
        } else {
            LOG_WARN(logger, "[message/group-text] 消息未被接受, from_uid: {}, group_id: {}, error: {}", actor_uid,
                     send.group_id(), accepted.error);
        }
        co_return result;
    }

    response->set_message_id(accepted.messageId);
    response->set_conversation_id(accepted.conversationId);
    response->set_conversation_seq(accepted.conversationSeq);
    auto* push = result.forward_packet.mutable_text_message_push();
    push->set_message_id(accepted.messageId);
    push->set_from(actor_uid);
    push->set_group_id(send.group_id());
    push->set_content(send.content());
    push->set_client_message_id(send.client_message_id());
    push->set_conversation_id(accepted.conversationId);
    push->set_conversation_seq(accepted.conversationSeq);
    push->set_send_date_time(send_date_time);
    result.recipients = std::move(accepted.recipientUids);
    result.should_forward = !accepted.duplicate;
    LOG_INFO(logger,
             "[message/group-text] 消息已保存, message_id: {}, conversation_id: {}, conversation_seq: {}, "
             "from_uid: {}, group_id: {}, recipients: {}, duplicate: {}",
             accepted.messageId, accepted.conversationId, accepted.conversationSeq, actor_uid, send.group_id(),
             result.recipients.size(), accepted.duplicate);
    co_return result;
}

seastar::future<acknowledged_message> messages::acknowledge(int64_t actor_uid, const protocol::Packet& request,
                                                            const std::string& device_id)
{
    acknowledged_message result;
    const auto& receipt = request.receipt();
    if (receipt.seq() <= 0 || receipt.conversation_id() <= 0 || receipt.conversation_seq() <= 0 ||
        (receipt.receipt_type() != protocol::RECEIPT_TYPE_DELIVERED &&
         receipt.receipt_type() != protocol::RECEIPT_TYPE_READ)) {
        LOG_WARN(logger,
                 "[message/receipt] 拒绝无效回执, uid: {}, message_id: {}, conversation_id: {}, "
                 "conversation_seq: {}",
                 actor_uid, receipt.seq(), receipt.conversation_id(), receipt.conversation_seq());
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser);
        co_return result;
    }

    const short message_status = receipt.receipt_type() == protocol::RECEIPT_TYPE_READ ? db::Message::Status::READ
                                                                                       : db::Message::Status::DELIVERED;
    const std::string read_time =
        receipt.receipt_type() == protocol::RECEIPT_TYPE_READ ? getCurrentDateTime() : std::string{};
    message_io::acknowledge_conversation_message operation_acknowledge_conversation_message{
        receipt.seq(),  actor_uid, device_id, receipt.conversation_id(), receipt.conversation_seq(),
        message_status, read_time};
    const auto acknowledged = co_await _io.submit(std::move(operation_acknowledge_conversation_message));
    if (acknowledged.updated <= 0) {
        LOG_WARN(logger, "[message/receipt] 回执更新失败或消息不属于当前用户, uid: {}, message_id: {}, result: {}",
                 actor_uid, receipt.seq(), acknowledged.updated);
        SetTcpPacketStatus(result.response,
                           acknowledged.updated == -1 ? ErrorCodes::MysqlFailed : ErrorCodes::MessageOwnershipInvalid);
        co_return result;
    }

    SetTcpPacketStatus(result.response, ErrorCodes::Success);
    if (receipt.receipt_type() == protocol::RECEIPT_TYPE_READ && acknowledged.directConversation &&
        acknowledged.readStateChanged) {
        result.sender_uid = acknowledged.senderUid;
        result.should_forward_read = result.sender_uid > 0;
        auto* push = result.read_receipt.mutable_read_receipt_push();
        push->set_message_id(receipt.seq());
        push->set_from(actor_uid);
        push->set_to(result.sender_uid);
        push->set_conversation_id(receipt.conversation_id());
        push->set_conversation_seq(receipt.conversation_seq());
    }
    LOG_DEBUG(logger, "[message/receipt] 回执已更新, uid: {}, message_id: {}, receipt_type: {}, forward_read: {}",
              actor_uid, receipt.seq(), static_cast<int>(receipt.receipt_type()), result.should_forward_read);
    co_return result;
}

seastar::future<accepted_message> messages::accept_file(int64_t actor_uid, const protocol::Packet& request)
{
    accepted_message result;
    const auto& send = request.send_file_request();
    auto* response = result.response.mutable_send_message_response();
    if (actor_uid <= 0 || send.to() <= 0 || !send.has_file() || send.client_message_id().empty()) {
        LOG_WARN(logger, "[message/direct-file] 拒绝无效消息, from_uid: {}, to_uid: {}", actor_uid, send.to());
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser, "to, file and client_message_id are required");
        co_return result;
    }
    message_io::find_ready_file operation_find_ready_file{send.file().file_id(), actor_uid};
    const auto file = co_await _io.submit(std::move(operation_find_ready_file));
    if (!file) {
        LOG_WARN(logger, "[message/direct-file] 文件未就绪, from_uid: {}, to_uid: {}, file_id: {}", actor_uid,
                 send.to(), send.file().file_id());
        SetTcpPacketStatus(result.response, ErrorCodes::NotFound, "ready file not found");
        co_return result;
    }
    message_io::accept_direct_text operation_accept_direct_text{
        actor_uid, send.to(), send.client_message_id(), file->fileId, getCurrentDateTime(), db::Message::Type::FILE};
    const auto accepted = co_await _io.submit(std::move(operation_accept_direct_text));
    SetTcpPacketStatus(result.response, accepted.error, accepted.diagnostic);
    if (accepted.error != ErrorCodes::Success) {
        LOG_ERROR(logger, "[message/direct-file] 文件消息保存失败, from_uid: {}, to_uid: {}, error: {}", actor_uid,
                  send.to(), accepted.error);
        co_return result;
    }

    response->set_message_id(accepted.messageId);
    response->set_conversation_id(accepted.conversationId);
    response->set_conversation_seq(accepted.conversationSeq);
    auto* push = result.forward_packet.mutable_file_message_push();
    push->set_message_id(accepted.messageId);
    push->set_from(actor_uid);
    push->set_to(send.to());
    fill_file_info(push->mutable_file(), *file);
    push->set_client_message_id(send.client_message_id());
    push->set_conversation_id(accepted.conversationId);
    push->set_conversation_seq(accepted.conversationSeq);
    result.recipients.push_back(send.to());
    result.should_forward = !accepted.duplicate;
    LOG_INFO(logger,
             "[message/direct-file] 文件消息已保存, message_id: {}, conversation_id: {}, from_uid: {}, "
             "to_uid: {}, file_id: {}, duplicate: {}",
             accepted.messageId, accepted.conversationId, actor_uid, send.to(), file->fileId, accepted.duplicate);
    co_return result;
}

seastar::future<accepted_message> messages::accept_group_file(int64_t actor_uid, const protocol::Packet& request)
{
    accepted_message result;
    const auto& send = request.send_group_file_request();
    auto* response = result.response.mutable_send_message_response();
    if (actor_uid <= 0 || send.group_id() <= 0 || !send.has_file() || send.client_message_id().empty()) {
        LOG_WARN(logger, "[message/group-file] 拒绝无效消息, from_uid: {}, group_id: {}", actor_uid, send.group_id());
        SetTcpPacketStatus(result.response, ErrorCodes::JsonParser,
                           "group_id, file and client_message_id are required");
        co_return result;
    }
    message_io::find_ready_file operation_find_ready_file{send.file().file_id(), actor_uid};
    const auto file = co_await _io.submit(std::move(operation_find_ready_file));
    if (!file) {
        LOG_WARN(logger, "[message/group-file] 文件未就绪, from_uid: {}, group_id: {}, file_id: {}", actor_uid,
                 send.group_id(), send.file().file_id());
        SetTcpPacketStatus(result.response, ErrorCodes::NotFound, "ready file not found");
        co_return result;
    }
    message_io::accept_group_message operation_accept_group_message{
        actor_uid,    send.group_id(),         send.client_message_id(),
        file->fileId, db::Message::Type::FILE, getCurrentDateTime(),
        true};
    const auto accepted = co_await _io.submit(std::move(operation_accept_group_message));
    SetTcpPacketStatus(result.response, accepted.error, accepted.diagnostic);
    if (accepted.error != ErrorCodes::Success) {
        LOG_ERROR(logger, "[message/group-file] 文件消息保存失败, from_uid: {}, group_id: {}, error: {}", actor_uid,
                  send.group_id(), accepted.error);
        co_return result;
    }

    response->set_message_id(accepted.messageId);
    response->set_conversation_id(accepted.conversationId);
    response->set_conversation_seq(accepted.conversationSeq);
    auto* push = result.forward_packet.mutable_file_message_push();
    push->set_message_id(accepted.messageId);
    push->set_from(actor_uid);
    push->set_group_id(send.group_id());
    fill_file_info(push->mutable_file(), *file);
    push->set_client_message_id(send.client_message_id());
    push->set_conversation_id(accepted.conversationId);
    push->set_conversation_seq(accepted.conversationSeq);
    result.recipients = accepted.recipientUids;
    result.should_forward = !accepted.duplicate;
    LOG_INFO(logger,
             "[message/group-file] 文件消息已保存, message_id: {}, conversation_id: {}, from_uid: {}, "
             "group_id: {}, file_id: {}, recipients: {}, duplicate: {}",
             accepted.messageId, accepted.conversationId, actor_uid, send.group_id(), file->fileId,
             result.recipients.size(), accepted.duplicate);
    co_return result;
}

seastar::future<protocol::Packet> messages::pull_conversation(int64_t actor_uid, const protocol::Packet& request)
{
    TcpPacket rsp;
    const auto& pull = request.pull_conversation_messages_request();
    if (pull.conversation_id() <= 0) {
        LOG_WARN(logger, "[message/history] 拒绝无效拉取请求, uid: {}, conversation_id: {}", actor_uid,
                 pull.conversation_id());
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        co_return rsp;
    }

    const int limit = pull.limit() > 0 ? pull.limit() : 50;
    message_io::sync_conversation operation_sync_conversation{actor_uid, pull.conversation_id(), pull.after_seq(),
                                                              limit};
    auto sync = co_await _io.submit(std::move(operation_sync_conversation));
    auto* response = rsp.mutable_pull_conversation_messages_response();
    response->set_conversation_id(pull.conversation_id());
    response->set_latest_seq(sync.latestSeq);
    response->set_next_seq(sync.nextSeq);
    response->set_has_more(sync.hasMore);
    if (sync.conversationType == 1) {
        response->set_conversation_type(protocol::CONVERSATION_TYPE_DIRECT);
    } else if (sync.conversationType == 2) {
        response->set_conversation_type(protocol::CONVERSATION_TYPE_GROUP);
    }
    SetTcpPacketStatus(rsp, sync.error);
    if (sync.error != ErrorCodes::Success) {
        LOG_WARN(logger, "[message/history] 消息记录读取失败, uid: {}, conversation_id: {}, error: {}", actor_uid,
                 pull.conversation_id(), sync.error);
        co_return rsp;
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
            fill_file_info(file, *message.file);
            file->set_expired(message.file->expired);
        }
    }
    LOG_DEBUG(logger,
              "[message/history] 消息记录读取成功, uid: {}, conversation_id: {}, messages: {}, "
              "next_seq: {}, has_more: {}",
              actor_uid, pull.conversation_id(), sync.messages.size(), sync.nextSeq, sync.hasMore);
    co_return rsp;
}

} // namespace imm::message_transaction
