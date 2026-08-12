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
    clientForwardService.ForwardToGateway(masterUid, SerializeTcpPacket(notice), ID_GROUP_NOTIFY_JOIN_REQ);
}

void GroupService::NotifyChange(const std::vector<int64_t>& recipients, int64_t groupId, int64_t targetUid,
                                int64_t actorUid, protocol::GroupChangeKind change, const std::string& groupName,
                                bool accept)
{
    for (const auto uid : recipients) {
        TcpPacket notice;
        auto* push = notice.mutable_group_change_push();
        push->set_group_id(groupId);
        push->set_requestor_uid(targetUid);
        push->set_replyor_uid(actorUid);
        push->set_accept(accept);
        push->set_change(change);
        push->set_group_name(groupName);
        clientForwardService.ForwardToGateway(uid, SerializeTcpPacket(notice), ID_GROUP_REPLY_JOIN_REQ);
    }
}

TcpPacket GroupService::Create(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& create = request.create_group_request();
    if (actorUid <= 0 || create.group_name().empty()) {
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
    return rsp;
}

TcpPacket GroupService::NotifyJoin(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& join = request.join_group_request();
    if (actorUid <= 0 || join.group_id() <= 0) {
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        return rsp;
    }
    const auto result = db::MysqlDao::GetInstance()->requestGroupJoin(join.group_id(), actorUid, join.request_message(),
                                                                      getCurrentDateTime());
    rsp.mutable_join_group_response();
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error == ErrorCodes::Success && !result.recipientUids.empty())
        NotifyJoinRequest(result.masterUid, actorUid, join.group_id(), join.request_message());
    return rsp;
}

TcpPacket GroupService::PullNotify(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& pull = request.pull_group_members_request();
    if (actorUid <= 0 || pull.group_id() <= 0) {
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
    if (result.error != ErrorCodes::Success)
        return rsp;
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
    return rsp;
}

TcpPacket GroupService::ReplyJoin(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& reply = request.reply_join_group_request();
    if (actorUid <= 0 || reply.group_id() <= 0 || reply.requestor_uid() <= 0 || !reply.has_accept()) {
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        return rsp;
    }
    const auto result = db::MysqlDao::GetInstance()->replyGroupJoin(reply.group_id(), actorUid, reply.requestor_uid(),
                                                                    reply.accept(), getCurrentDateTime());
    rsp.mutable_reply_join_group_response();
    SetTcpPacketStatus(rsp, result.error, result.diagnostic);
    if (result.error != ErrorCodes::Success)
        return rsp;
    NotifyChange(result.recipientUids, result.groupId, result.targetUid, result.actorUid,
                 reply.accept() ? protocol::GROUP_CHANGE_KIND_JOINED : protocol::GROUP_CHANGE_KIND_UNSPECIFIED,
                 result.groupName, reply.accept());
    return rsp;
}

TcpPacket GroupService::Quit(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto groupId = request.quit_group_request().group_id();
    if (actorUid <= 0 || groupId <= 0) {
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
    return rsp;
}

TcpPacket GroupService::Kick(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& kick = request.kick_group_member_request();
    if (actorUid <= 0 || kick.group_id() <= 0 || kick.member_uid() <= 0) {
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        return rsp;
    }
    if (actorUid == kick.member_uid()) {
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
    return rsp;
}

} // namespace wimi
