#include "GroupService.h"

#include "ClientForwardService.h"
#include "Const.h"
#include "Logger.h"
#include "Mysql.h"
#include "Redis.h"

namespace wimi
{

GroupService::GroupService(ClientForwardService& clientForwardService) : clientForwardService(clientForwardService) {}

void GroupService::NotifyJoinRequest(int64_t masterUid, int64_t requestorUid, int64_t groupId,
                                     const std::string& requestMessage)
{
    TcpPacket notice;
    auto* push = notice.mutable_group_join_request_push();
    push->set_requestor_uid(requestorUid);
    push->set_group_id(groupId);
    push->set_request_message(requestMessage);
    const bool forwarded =
        clientForwardService.ForwardToGateway(masterUid, SerializeTcpPacket(notice), ID_GROUP_NOTIFY_JOIN_REQ);
    LOG_DEBUG(logger,
              "[message/group-join-notify] 入群申请通知处理完成, group_id: {}, requestor_uid: {}, master_uid: {}, "
              "forwarded: {}",
              groupId, requestorUid, masterUid, forwarded);
}

void GroupService::NotifyChange(const std::vector<int64_t>& recipients, int64_t groupId, int64_t targetUid,
                                int64_t actorUid, protocol::GroupChangeKind change, const std::string& groupName,
                                bool accept)
{
    std::size_t forwarded = 0;
    for (const auto uid : recipients) {
        TcpPacket notice;
        auto* push = notice.mutable_group_change_push();
        push->set_group_id(groupId);
        push->set_requestor_uid(targetUid);
        push->set_replyor_uid(actorUid);
        push->set_accept(accept);
        push->set_change(change);
        push->set_group_name(groupName);
        if (clientForwardService.ForwardToGateway(uid, SerializeTcpPacket(notice), ID_GROUP_REPLY_JOIN_REQ))
            ++forwarded;
    }
    LOG_DEBUG(logger,
              "[message/group-change-notify] 群变更通知处理完成, group_id: {}, target_uid: {}, recipients: {}, "
              "forwarded: {}",
              groupId, targetUid, recipients.size(), forwarded);
}

TcpPacket GroupService::Create(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& create = request.create_group_request();
    if (actorUid <= 0 || create.group_name().empty()) {
        LOG_WARN(logger, "[message/group-create] 拒绝无效请求, uid: {}", actorUid);
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        return rsp;
    }
    const auto groupId = db::RedisDao::GetInstance()->generateGroupId();
    const auto conversationId = db::RedisDao::GetInstance()->generateConversationId();
    const db::GroupManager group(groupId, actorUid, conversationId, create.group_name(), getCurrentDateTime());
    const auto result = db::MysqlDao::GetInstance()->createGroup(group);
    auto* response = rsp.mutable_create_group_response();
    response->set_group_id(groupId);
    response->set_conversation_id(conversationId);
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error == ErrorCodes::Success) {
        LOG_INFO(logger, "[message/group-create] 群聊创建成功, uid: {}, group_id: {}, conversation_id: {}", actorUid,
                 groupId, conversationId);
    } else {
        LOG_WARN(logger, "[message/group-create] 群聊创建失败, uid: {}, error: {}", actorUid, result.error);
    }
    return rsp;
}

TcpPacket GroupService::NotifyJoin(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& join = request.join_group_request();
    if (actorUid <= 0 || join.group_id() <= 0) {
        LOG_WARN(logger, "[message/group-join] 拒绝无效请求, uid: {}, group_id: {}", actorUid, join.group_id());
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        return rsp;
    }
    const auto result = db::MysqlDao::GetInstance()->requestGroupJoin(join.group_id(), actorUid, join.request_message(),
                                                                      getCurrentDateTime());
    rsp.mutable_join_group_response();
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error == ErrorCodes::Success && !result.recipientUids.empty())
        NotifyJoinRequest(result.masterUid, actorUid, join.group_id(), join.request_message());
    if (result.error == ErrorCodes::Success) {
        LOG_INFO(logger, "[message/group-join] 入群申请已保存, uid: {}, group_id: {}", actorUid, join.group_id());
    } else {
        LOG_WARN(logger, "[message/group-join] 入群申请失败, uid: {}, group_id: {}, error: {}", actorUid,
                 join.group_id(), result.error);
    }
    return rsp;
}

TcpPacket GroupService::PullNotify(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& pull = request.pull_group_members_request();
    if (actorUid <= 0 || pull.group_id() <= 0) {
        LOG_WARN(logger, "[message/group-pull] 拒绝无效请求, uid: {}, group_id: {}", actorUid, pull.group_id());
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        return rsp;
    }
    const auto result = db::MysqlDao::GetInstance()->pullGroup(pull.group_id(), actorUid);
    auto* response = rsp.mutable_pull_group_members_response();
    response->set_group_id(pull.group_id());
    response->set_conversation_id(result.conversationId);
    response->set_master_uid(result.masterUid);
    response->set_group_name(result.groupName);
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error != ErrorCodes::Success) {
        LOG_WARN(logger, "[message/group-pull] 群信息读取失败, uid: {}, group_id: {}, error: {}", actorUid,
                 pull.group_id(), result.error);
        return rsp;
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
              actorUid, pull.group_id(), result.members.size(), result.applies.size());
    return rsp;
}

TcpPacket GroupService::ReplyJoin(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& reply = request.reply_join_group_request();
    if (actorUid <= 0 || reply.group_id() <= 0 || reply.requestor_uid() <= 0 || !reply.has_accept()) {
        LOG_WARN(logger, "[message/group-join-reply] 拒绝无效请求, uid: {}, group_id: {}, requestor_uid: {}", actorUid,
                 reply.group_id(), reply.requestor_uid());
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        return rsp;
    }
    const auto result = db::MysqlDao::GetInstance()->replyGroupJoin(reply.group_id(), actorUid, reply.requestor_uid(),
                                                                    reply.accept(), getCurrentDateTime());
    rsp.mutable_reply_join_group_response();
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error != ErrorCodes::Success) {
        LOG_WARN(logger,
                 "[message/group-join-reply] 入群申请处理失败, uid: {}, group_id: {}, requestor_uid: {}, "
                 "error: {}",
                 actorUid, reply.group_id(), reply.requestor_uid(), result.error);
        return rsp;
    }
    NotifyChange(result.recipientUids, result.groupId, result.targetUid, result.actorUid,
                 reply.accept() ? protocol::GROUP_CHANGE_KIND_JOINED : protocol::GROUP_CHANGE_KIND_UNSPECIFIED,
                 result.groupName, reply.accept());
    LOG_INFO(logger,
             "[message/group-join-reply] 入群申请已处理, uid: {}, group_id: {}, requestor_uid: {}, accepted: {}",
             actorUid, reply.group_id(), reply.requestor_uid(), reply.accept());
    return rsp;
}

TcpPacket GroupService::Quit(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto groupId = request.quit_group_request().group_id();
    if (actorUid <= 0 || groupId <= 0) {
        LOG_WARN(logger, "[message/group-quit] 拒绝无效请求, uid: {}, group_id: {}", actorUid, groupId);
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        return rsp;
    }
    const auto result = db::MysqlDao::GetInstance()->removeGroupMember(groupId, actorUid, actorUid, true);
    rsp.mutable_quit_group_response();
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error == ErrorCodes::Success)
        NotifyChange(result.recipientUids, result.groupId, result.targetUid, result.actorUid,
                     result.targetUid == result.masterUid ? protocol::GROUP_CHANGE_KIND_DISSOLVED
                                                          : protocol::GROUP_CHANGE_KIND_LEFT,
                     result.groupName);
    if (result.error == ErrorCodes::Success) {
        LOG_INFO(logger, "[message/group-quit] 用户已退出群聊, uid: {}, group_id: {}", actorUid, groupId);
    } else {
        LOG_WARN(logger, "[message/group-quit] 退出群聊失败, uid: {}, group_id: {}, error: {}", actorUid, groupId,
                 result.error);
    }
    return rsp;
}

TcpPacket GroupService::Kick(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& kick = request.kick_group_member_request();
    if (actorUid <= 0 || kick.group_id() <= 0 || kick.member_uid() <= 0) {
        LOG_WARN(logger, "[message/group-kick] 拒绝无效请求, uid: {}, group_id: {}, member_uid: {}", actorUid,
                 kick.group_id(), kick.member_uid());
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        return rsp;
    }
    if (actorUid == kick.member_uid()) {
        LOG_WARN(logger, "[message/group-kick] 群主不能移除自己, uid: {}, group_id: {}", actorUid, kick.group_id());
        SetTcpPacketStatus(rsp, ErrorCodes::MessageOwnershipInvalid, "master cannot kick itself");
        return rsp;
    }
    const auto result =
        db::MysqlDao::GetInstance()->removeGroupMember(kick.group_id(), actorUid, kick.member_uid(), false);
    rsp.mutable_kick_group_member_response();
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error == ErrorCodes::Success)
        NotifyChange(result.recipientUids, result.groupId, result.targetUid, result.actorUid,
                     protocol::GROUP_CHANGE_KIND_KICKED, result.groupName);
    if (result.error == ErrorCodes::Success) {
        LOG_INFO(logger, "[message/group-kick] 群成员已移除, uid: {}, group_id: {}, member_uid: {}", actorUid,
                 kick.group_id(), kick.member_uid());
    } else {
        LOG_WARN(logger, "[message/group-kick] 移除群成员失败, uid: {}, group_id: {}, member_uid: {}, error: {}",
                 actorUid, kick.group_id(), kick.member_uid(), result.error);
    }
    return rsp;
}

} // namespace wimi
