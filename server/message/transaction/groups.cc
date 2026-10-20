#include "transaction/groups.hh"

#include "Const.h"
#include "Logger.h"

namespace wimi::message_transaction
{

void groups::notify_application(request_context& context, int64_t master_uid, int64_t requestor_uid, int64_t group_id,
                                const std::string& request_message)
{
    TcpPacket notice;
    auto* push = notice.mutable_group_join_request_push();
    push->set_requestor_uid(requestor_uid);
    push->set_group_id(group_id);
    push->set_request_message(request_message);
    context.deliveries.push_back({master_uid, std::move(notice), {}});
    LOG_DEBUG(logger, "[message/group-join-notify] 入群申请通知已暂存, group_id: {}, requestor_uid: {}, master_uid: {}",
              group_id, requestor_uid, master_uid);
}

void groups::notify_change(request_context& context, const std::vector<int64_t>& recipients, int64_t group_id,
                           int64_t target_uid, int64_t actor_uid, protocol::GroupChangeKind change,
                           const std::string& group_name, bool accept)
{
    for (const auto uid : recipients) {
        TcpPacket notice;
        auto* push = notice.mutable_group_change_push();
        push->set_group_id(group_id);
        push->set_requestor_uid(target_uid);
        push->set_replyor_uid(actor_uid);
        push->set_accept(accept);
        push->set_change(change);
        push->set_group_name(group_name);
        context.deliveries.push_back({uid, std::move(notice), {}});
    }
    LOG_DEBUG(logger, "[message/group-change-notify] 群变更通知已暂存, group_id: {}, target_uid: {}, recipients: {}",
              group_id, target_uid, recipients.size());
}

seastar::future<protocol::Packet> groups::create(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    const auto& request = context.packet;
    TcpPacket rsp;
    const auto& create = request.create_group_request();
    if (actor_uid <= 0 || create.group_name().empty()) {
        LOG_WARN(logger, "[message/group-create] 拒绝无效请求, uid: {}", actor_uid);
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        co_return rsp;
    }
    message_io::generate_id group_id_operation{message_io::id_kind::group};
    const auto group_id = co_await _io.submit(std::move(group_id_operation));
    message_io::generate_id conversation_id_operation{message_io::id_kind::conversation};
    const auto conversation_id = co_await _io.submit(std::move(conversation_id_operation));
    const db::GroupManager group(group_id, actor_uid, conversation_id, create.group_name(), getCurrentDateTime());
    message_io::create_group create_group_operation{group};
    const auto result = co_await _io.submit(std::move(create_group_operation));
    auto* response = rsp.mutable_create_group_response();
    response->set_group_id(group_id);
    response->set_conversation_id(conversation_id);
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error == ErrorCodes::Success) {
        LOG_INFO(logger, "[message/group-create] 群聊创建成功, uid: {}, group_id: {}, conversation_id: {}", actor_uid,
                 group_id, conversation_id);
    } else {
        LOG_WARN(logger, "[message/group-create] 群聊创建失败, uid: {}, error: {}", actor_uid, result.error);
    }
    co_return rsp;
}

seastar::future<protocol::Packet> groups::apply(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    const auto& request = context.packet;
    TcpPacket rsp;
    const auto& join = request.join_group_request();
    if (actor_uid <= 0 || join.group_id() <= 0) {
        LOG_WARN(logger, "[message/group-join] 拒绝无效请求, uid: {}, group_id: {}", actor_uid, join.group_id());
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        co_return rsp;
    }
    message_io::request_group_join request_group_join_operation{join.group_id(), actor_uid, join.request_message(),
                                                                getCurrentDateTime()};
    const auto result = co_await _io.submit(std::move(request_group_join_operation));
    rsp.mutable_join_group_response();
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error == ErrorCodes::Success && !result.recipientUids.empty()) {
        notify_application(context, result.masterUid, actor_uid, join.group_id(), join.request_message());
    }
    if (result.error == ErrorCodes::Success) {
        LOG_INFO(logger, "[message/group-join] 入群申请已保存, uid: {}, group_id: {}", actor_uid, join.group_id());
    } else {
        LOG_WARN(logger, "[message/group-join] 入群申请失败, uid: {}, group_id: {}, error: {}", actor_uid,
                 join.group_id(), result.error);
    }
    co_return rsp;
}

seastar::future<protocol::Packet> groups::pull(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    const auto& request = context.packet;
    TcpPacket rsp;
    const auto& pull = request.pull_group_members_request();
    if (actor_uid <= 0 || pull.group_id() <= 0) {
        LOG_WARN(logger, "[message/group-pull] 拒绝无效请求, uid: {}, group_id: {}", actor_uid, pull.group_id());
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        co_return rsp;
    }
    message_io::pull_group pull_group_operation{pull.group_id(), actor_uid};
    const auto result = co_await _io.submit(std::move(pull_group_operation));
    auto* response = rsp.mutable_pull_group_members_response();
    response->set_group_id(pull.group_id());
    response->set_conversation_id(result.conversationId);
    response->set_master_uid(result.masterUid);
    response->set_group_name(result.groupName);
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error != ErrorCodes::Success) {
        LOG_WARN(logger, "[message/group-pull] 群信息读取失败, uid: {}, group_id: {}, error: {}", actor_uid,
                 pull.group_id(), result.error);
        co_return rsp;
    }
    for (const auto& member : result.members) {
        auto* item = response->add_members();
        item->set_uid(member.uid);
        item->set_name(member.memberName);
        item->set_join_time(member.joinTime);
    }
    for (const auto& apply : result.applies) {
        auto* item = response->add_applications();
        item->set_requestor_uid(apply.requestor);
        item->set_request_message(apply.message);
        item->set_create_time(apply.createTime);
    }
    LOG_DEBUG(logger, "[message/group-pull] 群信息读取成功, uid: {}, group_id: {}, members: {}, applications: {}",
              actor_uid, pull.group_id(), result.members.size(), result.applies.size());
    co_return rsp;
}

seastar::future<protocol::Packet> groups::reply(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    const auto& request = context.packet;
    TcpPacket rsp;
    const auto& reply = request.reply_join_group_request();
    if (actor_uid <= 0 || reply.group_id() <= 0 || reply.requestor_uid() <= 0 || !reply.has_accept()) {
        LOG_WARN(logger, "[message/group-join-reply] 拒绝无效请求, uid: {}, group_id: {}, requestor_uid: {}", actor_uid,
                 reply.group_id(), reply.requestor_uid());
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        co_return rsp;
    }
    message_io::reply_group_join reply_group_join_operation{reply.group_id(), actor_uid, reply.requestor_uid(),
                                                            reply.accept(), getCurrentDateTime()};
    const auto result = co_await _io.submit(std::move(reply_group_join_operation));
    rsp.mutable_reply_join_group_response();
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error != ErrorCodes::Success) {
        LOG_WARN(logger,
                 "[message/group-join-reply] 入群申请处理失败, uid: {}, group_id: {}, requestor_uid: {}, "
                 "error: {}",
                 actor_uid, reply.group_id(), reply.requestor_uid(), result.error);
        co_return rsp;
    }
    notify_change(context, result.recipientUids, result.groupId, result.targetUid, result.actorUid,
                  reply.accept() ? protocol::GROUP_CHANGE_KIND_JOINED : protocol::GROUP_CHANGE_KIND_UNSPECIFIED,
                  result.groupName, reply.accept());
    LOG_INFO(logger,
             "[message/group-join-reply] 入群申请已处理, uid: {}, group_id: {}, requestor_uid: {}, accepted: {}",
             actor_uid, reply.group_id(), reply.requestor_uid(), reply.accept());
    co_return rsp;
}

seastar::future<protocol::Packet> groups::quit(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    const auto& request = context.packet;
    TcpPacket rsp;
    const auto group_id = request.quit_group_request().group_id();
    if (actor_uid <= 0 || group_id <= 0) {
        LOG_WARN(logger, "[message/group-quit] 拒绝无效请求, uid: {}, group_id: {}", actor_uid, group_id);
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        co_return rsp;
    }
    message_io::remove_group_member remove_group_member_operation{group_id, actor_uid, actor_uid, true};
    const auto result = co_await _io.submit(std::move(remove_group_member_operation));
    rsp.mutable_quit_group_response();
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error == ErrorCodes::Success) {
        notify_change(context, result.recipientUids, result.groupId, result.targetUid, result.actorUid,
                      result.targetUid == result.masterUid ? protocol::GROUP_CHANGE_KIND_DISSOLVED
                                                           : protocol::GROUP_CHANGE_KIND_LEFT,
                      result.groupName);
    }
    if (result.error == ErrorCodes::Success) {
        LOG_INFO(logger, "[message/group-quit] 用户已退出群聊, uid: {}, group_id: {}", actor_uid, group_id);
    } else {
        LOG_WARN(logger, "[message/group-quit] 退出群聊失败, uid: {}, group_id: {}, error: {}", actor_uid, group_id,
                 result.error);
    }
    co_return rsp;
}

seastar::future<protocol::Packet> groups::kick(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    const auto& request = context.packet;
    TcpPacket rsp;
    const auto& kick = request.kick_group_member_request();
    if (actor_uid <= 0 || kick.group_id() <= 0 || kick.member_uid() <= 0) {
        LOG_WARN(logger, "[message/group-kick] 拒绝无效请求, uid: {}, group_id: {}, member_uid: {}", actor_uid,
                 kick.group_id(), kick.member_uid());
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        co_return rsp;
    }
    if (actor_uid == kick.member_uid()) {
        LOG_WARN(logger, "[message/group-kick] 群主不能移除自己, uid: {}, group_id: {}", actor_uid, kick.group_id());
        SetTcpPacketStatus(rsp, ErrorCodes::MessageOwnershipInvalid, "master cannot kick itself");
        co_return rsp;
    }
    message_io::remove_group_member remove_group_member_operation{kick.group_id(), actor_uid, kick.member_uid(), false};
    const auto result = co_await _io.submit(std::move(remove_group_member_operation));
    rsp.mutable_kick_group_member_response();
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error == ErrorCodes::Success) {
        notify_change(context, result.recipientUids, result.groupId, result.targetUid, result.actorUid,
                      protocol::GROUP_CHANGE_KIND_KICKED, result.groupName);
    }
    if (result.error == ErrorCodes::Success) {
        LOG_INFO(logger, "[message/group-kick] 群成员已移除, uid: {}, group_id: {}, member_uid: {}", actor_uid,
                 kick.group_id(), kick.member_uid());
    } else {
        LOG_WARN(logger, "[message/group-kick] 移除群成员失败, uid: {}, group_id: {}, member_uid: {}, error: {}",
                 actor_uid, kick.group_id(), kick.member_uid(), result.error);
    }
    co_return rsp;
}

} // namespace wimi::message_transaction
