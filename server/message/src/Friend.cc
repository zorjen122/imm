#include "FriendService.h"

#include "ClientForwardService.h"
#include "Const.h"
#include "DbGlobal.h"
#include "Logger.h"
#include "Mysql.h"
#include "Redis.h"

#include <spdlog/spdlog.h>
#include <string>

namespace wimi
{

FriendService::FriendService(ClientForwardService& clientForwardService) : clientForwardService(clientForwardService) {}

int FriendService::StoreNotifyAddFriend(int64_t actorUid, const protocol::AddFriendRequest& request)
{
    db::FriendApply::Ptr friendApply(
        new db::FriendApply(actorUid, request.to(), db::FriendApply::Status::Wait, request.request_message()));
    return db::MysqlDao::GetInstance()->insertFriendApply(friendApply);
}

TcpPacket FriendService::NotifyAddFriend(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& addFriend = request.add_friend_request();
    const int64_t to = addFriend.to();
    rsp.mutable_add_friend_response();

    if (StoreNotifyAddFriend(actorUid, addFriend) == -1) {
        LOG_ERROR(logger, "[message/friend-request] 保存好友申请失败, from_uid: {}, to_uid: {}", actorUid, to);
        SetTcpPacketStatus(rsp, ErrorCodes::MysqlFailed, "数据库修改异常");
        return rsp;
    }

    TcpPacket push;
    auto* notice = push.mutable_friend_request_push();
    notice->set_seq(db::RedisDao::GetInstance()->generateMsgId());
    notice->set_from(actorUid);
    notice->set_to(to);
    notice->set_request_message(addFriend.request_message());
    if (!clientForwardService.ForwardToGateway(to, SerializeTcpPacket(push), ID_NOTIFY_ADD_FRIEND_REQ)) {
        LOG_DEBUG(logger, "[message/friend-request] 好友申请已保存, 接收用户当前不在线, from_uid: {}, to_uid: {}",
                  actorUid, to);
    }
    LOG_INFO(logger, "[message/friend-request] 好友申请已保存, from_uid: {}, to_uid: {}", actorUid, to);
    SetTcpPacketStatus(rsp, ErrorCodes::Success);
    return rsp;
}

TcpPacket FriendService::ReplyAddFriend(unsigned int, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const auto& replyFriend = request.reply_friend_request();
    const int64_t to = replyFriend.to();
    rsp.mutable_reply_friend_response();
    if (!replyFriend.has_accept()) {
        LOG_WARN(logger, "[message/friend-reply] 拒绝缺少处理结果的请求, from_uid: {}, to_uid: {}", actorUid, to);
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser, "accept is required");
        return rsp;
    }

    if (StoreReplyAddFriend(actorUid, replyFriend) == -1) {
        LOG_ERROR(logger, "[message/friend-reply] 保存好友申请处理结果失败, from_uid: {}, to_uid: {}", actorUid, to);
        SetTcpPacketStatus(rsp, ErrorCodes::MysqlFailed, "数据库修改发送异常");
        return rsp;
    }

    TcpPacket push;
    auto* notice = push.mutable_friend_reply_push();
    notice->set_seq(db::RedisDao::GetInstance()->generateMsgId());
    notice->set_from(actorUid);
    notice->set_to(to);
    notice->set_accept(replyFriend.accept());
    notice->set_reply_message(replyFriend.reply_message());
    if (!clientForwardService.ForwardToGateway(to, SerializeTcpPacket(push), ID_REPLY_ADD_FRIEND_REQ)) {
        LOG_DEBUG(logger, "[message/friend-reply] 处理结果已保存, 接收用户当前不在线, from_uid: {}, to_uid: {}",
                  actorUid, to);
    }
    LOG_INFO(logger, "[message/friend-reply] 好友申请已处理, from_uid: {}, to_uid: {}, accepted: {}", actorUid, to,
             replyFriend.accept());
    SetTcpPacketStatus(rsp, ErrorCodes::Success);
    return rsp;
}

int FriendService::StoreReplyAddFriend(int64_t actorUid, const protocol::ReplyFriendRequest& request)
{
    const int64_t to = request.to();
    int result = -1;
    db::FriendApply::Status status{};
    const std::string time = getCurrentDateTime();
    if (request.accept()) {
        status = db::FriendApply::Status::Agree;
        const auto friendshipId = db::RedisDao::GetInstance()->generateFriendshipId();
        const auto conversationId = db::RedisDao::GetInstance()->generateConversationId();
        db::Friend::Ptr friendData(new db::Friend(friendshipId, conversationId, actorUid, to, time));
        result = db::MysqlDao::GetInstance()->insertFriend(friendData);
        if (result == -1) {
            LOG_ERROR(logger, "[message/friend-reply] 创建好友关系失败, from_uid: {}, to_uid: {}", actorUid, to);
            return -1;
        }
    } else {
        status = db::FriendApply::Status::Refuse;
    }

    db::FriendApply::Ptr friendApply(new db::FriendApply(actorUid, to, status, request.reply_message(), time));
    result = db::MysqlDao::GetInstance()->updateFriendApplyStatus(friendApply);
    if (result == -1)
        LOG_ERROR(logger, "[message/friend-reply] 更新好友申请状态失败, from_uid: {}, to_uid: {}", actorUid, to);
    return result;
}

TcpPacket FriendService::PullFriendApplyList(uint32_t, int64_t actorUid, TcpPacket&)
{
    TcpPacket rsp;
    auto* payload = rsp.mutable_pull_friend_apply_list_response();
    db::FriendApply::FriendApplyGroup applyList = db::MysqlDao::GetInstance()->getFriendApplyList(actorUid);
    if (applyList == nullptr) {
        LOG_DEBUG(logger, "[message/friend-requests] 好友申请列表为空, uid: {}", actorUid);
        SetTcpPacketStatus(rsp, ErrorCodes::Success);
        return rsp;
    }
    LOG_DEBUG(logger, "[message/friend-requests] 好友申请列表读取成功, uid: {}, count: {}", actorUid,
              applyList->size());

    for (const auto& applyObject : *applyList) {
        auto* apply = payload->add_applications();
        apply->set_from(applyObject->from);
        apply->set_status(applyObject->status);
        apply->set_content(applyObject->content);
    }
    SetTcpPacketStatus(rsp, ErrorCodes::Success);
    return rsp;
}

TcpPacket FriendService::PullFriendList(uint32_t, int64_t actorUid, TcpPacket&)
{
    TcpPacket rsp;
    auto* payload = rsp.mutable_pull_friend_list_response();
    db::Friend::FriendGroup friendList = db::MysqlDao::GetInstance()->getFriendList(actorUid);
    if (friendList == nullptr) {
        LOG_DEBUG(logger, "[message/friends] 好友列表为空, uid: {}", actorUid);
        SetTcpPacketStatus(rsp, ErrorCodes::Success);
        return rsp;
    }
    LOG_DEBUG(logger, "[message/friends] 好友列表读取成功, uid: {}, records: {}", actorUid, friendList->size());

    for (const auto& friendObject : *friendList) {
        const int64_t friendUid = friendObject->uidB;
        const db::UserInfo::Ptr friendInfo = db::MysqlDao::GetInstance()->getUserInfo(friendUid);
        if (friendInfo != nullptr) {
            auto* info = payload->add_friends();
            info->set_uid(friendUid);
            info->set_name(friendInfo->name);
            info->set_age(friendInfo->age);
            info->set_sex(friendInfo->sex);
            info->set_head_image_url(friendInfo->headImageURL);
        }
    }
    SetTcpPacketStatus(rsp, ErrorCodes::Success);
    return rsp;
}

} // namespace wimi
