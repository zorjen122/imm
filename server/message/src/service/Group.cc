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
    notice.set_uid(requestorUid);
    notice.set_group_id(groupId);
    notice.set_content(requestMessage);
    clientForwardService.ForwardToGateway(masterUid, SerializeTcpPacket(notice), ID_GROUP_NOTIFY_JOIN_REQ);
}

void GroupService::NotifyChange(const std::vector<int64_t>& recipients, int64_t groupId, int64_t targetUid,
                                int64_t actorUid, protocol::GroupChangeKind change, const std::string& groupName,
                                bool accept)
{
    for (const auto uid : recipients) {
        TcpPacket notice;
        notice.set_group_id(groupId);
        notice.set_requestor_uid(targetUid);
        notice.set_replyor_uid(actorUid);
        notice.set_accept(accept);
        notice.set_group_change_kind(change);
        notice.set_group_name(groupName);
        clientForwardService.ForwardToGateway(uid, SerializeTcpPacket(notice), ID_GROUP_REPLY_JOIN_REQ);
    }
}

TcpPacket GroupService::Create(unsigned int, TcpPacket& request)
{
    TcpPacket rsp;
    if (request.uid() <= 0 || !request.has_group_name() || request.group_name().empty()) {
        rsp.set_error(ErrorCodes::JsonParser);
        return rsp;
    }
    const auto groupId = db::RedisDao::GetInstance()->generateGroupId();
    const auto conversationId = db::RedisDao::GetInstance()->generateConversationId();
    const db::GroupManager group(groupId, request.uid(), conversationId, request.group_name(), getCurrentDateTime());
    const auto result = db::MysqlDao::GetInstance()->createGroup(group);
    rsp.set_error(result.error);
    rsp.set_group_id(groupId);
    rsp.set_conversation_id(conversationId);
    rsp.set_group_name(request.group_name());
    rsp.set_message(result.diagnostic);
    return rsp;
}

TcpPacket GroupService::NotifyJoin(unsigned int, TcpPacket& request)
{
    TcpPacket rsp;
    if (request.uid() <= 0 || request.group_id() <= 0) {
        rsp.set_error(ErrorCodes::JsonParser);
        return rsp;
    }
    const auto result = db::MysqlDao::GetInstance()->requestGroupJoin(request.group_id(), request.uid(),
                                                                      request.request_message(), getCurrentDateTime());
    rsp.set_error(result.error);
    rsp.set_group_id(request.group_id());
    rsp.set_message(result.diagnostic);
    if (result.error == ErrorCodes::Success && !result.recipientUids.empty())
        NotifyJoinRequest(result.masterUid, request.uid(), request.group_id(), request.request_message());
    return rsp;
}

TcpPacket GroupService::PullNotify(unsigned int, TcpPacket& request)
{
    TcpPacket rsp;
    if (request.uid() <= 0 || request.group_id() <= 0) {
        rsp.set_error(ErrorCodes::JsonParser);
        return rsp;
    }
    const auto result = db::MysqlDao::GetInstance()->pullGroup(request.group_id(), request.uid());
    rsp.set_error(result.error);
    rsp.set_group_id(request.group_id());
    rsp.set_conversation_id(result.conversationId);
    rsp.set_replyor_uid(result.masterUid);
    rsp.set_group_name(result.groupName);
    rsp.set_message(result.diagnostic);
    if (result.error != ErrorCodes::Success)
        return rsp;
    for (const auto& member : result.members) {
        auto* item = rsp.add_member_list();
        item->set_uid(member.uid);
        item->set_name(member.memberName);
        item->set_join_time(member.joinTime);
    }
    for (const auto& apply : result.applies) {
        auto* item = rsp.add_apply_list();
        item->set_from(apply.requestor);
        item->set_to(result.masterUid);
        item->set_content(apply.message);
        item->set_create_time(apply.createTime);
    }
    return rsp;
}

TcpPacket GroupService::ReplyJoin(unsigned int, TcpPacket& request)
{
    TcpPacket rsp;
    if (request.uid() <= 0 || request.group_id() <= 0 || request.requestor_uid() <= 0 || !request.has_accept()) {
        rsp.set_error(ErrorCodes::JsonParser);
        return rsp;
    }
    const auto result = db::MysqlDao::GetInstance()->replyGroupJoin(
        request.group_id(), request.uid(), request.requestor_uid(), request.accept(), getCurrentDateTime());
    rsp.set_error(result.error);
    rsp.set_group_id(request.group_id());
    rsp.set_message(result.diagnostic);
    if (result.error != ErrorCodes::Success)
        return rsp;
    if (request.accept())
        NotifyChange(result.recipientUids, result.groupId, result.targetUid, result.actorUid,
                     protocol::GROUP_CHANGE_KIND_JOINED, result.groupName, true);
    else
        NotifyChange(result.recipientUids, result.groupId, result.targetUid, result.actorUid,
                     protocol::GROUP_CHANGE_KIND_UNSPECIFIED, result.groupName, false);
    return rsp;
}

TcpPacket GroupService::Quit(unsigned int, TcpPacket& request)
{
    TcpPacket rsp;
    if (request.uid() <= 0 || request.group_id() <= 0) {
        rsp.set_error(ErrorCodes::JsonParser);
        return rsp;
    }
    const auto result =
        db::MysqlDao::GetInstance()->removeGroupMember(request.group_id(), request.uid(), request.uid(), true);
    rsp.set_error(result.error);
    rsp.set_group_id(request.group_id());
    rsp.set_message(result.diagnostic);
    if (result.error == ErrorCodes::Success)
        NotifyChange(result.recipientUids, result.groupId, result.targetUid, result.actorUid,
                     result.targetUid == result.masterUid ? protocol::GROUP_CHANGE_KIND_DISSOLVED
                                                          : protocol::GROUP_CHANGE_KIND_LEFT,
                     result.groupName);
    return rsp;
}

TcpPacket GroupService::Kick(unsigned int, TcpPacket& request)
{
    TcpPacket rsp;
    if (request.uid() <= 0 || request.group_id() <= 0 || request.requestor_uid() <= 0) {
        rsp.set_error(ErrorCodes::JsonParser);
        return rsp;
    }
    if (request.uid() == request.requestor_uid()) {
        rsp.set_error(ErrorCodes::MessageOwnershipInvalid);
        rsp.set_message("master cannot kick itself");
        return rsp;
    }
    const auto result = db::MysqlDao::GetInstance()->removeGroupMember(request.group_id(), request.uid(),
                                                                       request.requestor_uid(), false);
    rsp.set_error(result.error);
    rsp.set_group_id(request.group_id());
    rsp.set_message(result.diagnostic);
    if (result.error == ErrorCodes::Success)
        NotifyChange(result.recipientUids, result.groupId, result.targetUid, result.actorUid,
                     protocol::GROUP_CHANGE_KIND_KICKED, result.groupName);
    return rsp;
}

} // namespace wimi
