#include "transaction/friends.hh"

#include "Const.h"
#include "DbGlobal.h"
#include "Logger.h"

#include <spdlog/spdlog.h>
#include <string>

namespace imm::message_transaction
{

seastar::future<int> friends::store_application(int64_t actor_uid, const protocol::AddFriendRequest& request)
{
    db::FriendApply::Ptr friend_apply(
        new db::FriendApply(actor_uid, request.to(), db::FriendApply::Status::Wait, request.request_message()));
    message_io::insert_friend_apply insert_friend_apply_operation{friend_apply};
    co_return co_await _io.submit(std::move(insert_friend_apply_operation));
}

seastar::future<protocol::Packet> friends::apply(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    const auto& request = context.packet;
    TcpPacket rsp;
    const auto& add_friend = request.add_friend_request();
    const int64_t to = add_friend.to();
    rsp.mutable_add_friend_response();

    message_io::generate_id message_id_operation{message_io::id_kind::message};
    const auto notification_id = co_await _io.submit(std::move(message_id_operation));
    if (co_await store_application(actor_uid, add_friend) == -1) {
        LOG_ERROR(logger, "[message/friend-request] 保存好友申请失败, from_uid: {}, to_uid: {}", actor_uid, to);
        SetTcpPacketStatus(rsp, ErrorCodes::MysqlFailed, "数据库修改异常");
        co_return rsp;
    }

    TcpPacket push;
    auto* notice = push.mutable_friend_request_push();
    notice->set_seq(notification_id);
    notice->set_from(actor_uid);
    notice->set_to(to);
    notice->set_request_message(add_friend.request_message());
    context.deliveries.push_back({to, std::move(push), {}});
    LOG_INFO(logger, "[message/friend-request] 好友申请已保存, from_uid: {}, to_uid: {}", actor_uid, to);
    SetTcpPacketStatus(rsp, ErrorCodes::Success);
    co_return rsp;
}

seastar::future<protocol::Packet> friends::reply(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    const auto& request = context.packet;
    TcpPacket rsp;
    const auto& reply_friend = request.reply_friend_request();
    const int64_t to = reply_friend.to();
    rsp.mutable_reply_friend_response();
    if (!reply_friend.has_accept()) {
        LOG_WARN(logger, "[message/friend-reply] 拒绝缺少处理结果的请求, from_uid: {}, to_uid: {}", actor_uid, to);
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser, "accept is required");
        co_return rsp;
    }

    message_io::generate_id message_id_operation{message_io::id_kind::message};
    const auto notification_id = co_await _io.submit(std::move(message_id_operation));
    if (co_await store_reply(actor_uid, reply_friend) == -1) {
        LOG_ERROR(logger, "[message/friend-reply] 保存好友申请处理结果失败, from_uid: {}, to_uid: {}", actor_uid, to);
        SetTcpPacketStatus(rsp, ErrorCodes::MysqlFailed, "数据库修改发送异常");
        co_return rsp;
    }

    TcpPacket push;
    auto* notice = push.mutable_friend_reply_push();
    notice->set_seq(notification_id);
    notice->set_from(actor_uid);
    notice->set_to(to);
    notice->set_accept(reply_friend.accept());
    notice->set_reply_message(reply_friend.reply_message());
    context.deliveries.push_back({to, std::move(push), {}});
    LOG_INFO(logger, "[message/friend-reply] 好友申请已处理, from_uid: {}, to_uid: {}, accepted: {}", actor_uid, to,
             reply_friend.accept());
    SetTcpPacketStatus(rsp, ErrorCodes::Success);
    co_return rsp;
}

seastar::future<int> friends::store_reply(int64_t actor_uid, const protocol::ReplyFriendRequest& request)
{
    const int64_t to = request.to();
    int result = -1;
    db::FriendApply::Status status{};
    const std::string time = getCurrentDateTime();
    if (request.accept()) {
        status = db::FriendApply::Status::Agree;
        message_io::generate_id friendship_id_operation{message_io::id_kind::friendship};
        const auto friendship_id = co_await _io.submit(std::move(friendship_id_operation));
        message_io::generate_id conversation_id_operation{message_io::id_kind::conversation};
        const auto conversation_id = co_await _io.submit(std::move(conversation_id_operation));
        db::Friend::Ptr friend_data(new db::Friend(friendship_id, conversation_id, actor_uid, to, time));
        message_io::insert_friend insert_friend_operation{friend_data};
        result = co_await _io.submit(std::move(insert_friend_operation));
        if (result == -1) {
            LOG_ERROR(logger, "[message/friend-reply] 创建好友关系失败, from_uid: {}, to_uid: {}", actor_uid, to);
            co_return -1;
        }
    } else {
        status = db::FriendApply::Status::Refuse;
    }

    db::FriendApply::Ptr friend_apply(new db::FriendApply(actor_uid, to, status, request.reply_message(), time));
    message_io::update_friend_apply_status update_friend_apply_status_operation{friend_apply};
    result = co_await _io.submit(std::move(update_friend_apply_status_operation));
    if (result == -1) {
        LOG_ERROR(logger, "[message/friend-reply] 更新好友申请状态失败, from_uid: {}, to_uid: {}", actor_uid, to);
    }
    co_return result;
}

seastar::future<protocol::Packet> friends::pull_applications(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    TcpPacket rsp;
    auto* payload = rsp.mutable_pull_friend_apply_list_response();
    message_io::get_friend_apply_list get_friend_apply_list_operation{actor_uid};
    db::FriendApply::FriendApplyGroup apply_list = co_await _io.submit(std::move(get_friend_apply_list_operation));
    if (apply_list == nullptr) {
        LOG_DEBUG(logger, "[message/friend-requests] 好友申请列表为空, uid: {}", actor_uid);
        SetTcpPacketStatus(rsp, ErrorCodes::Success);
        co_return rsp;
    }
    LOG_DEBUG(logger, "[message/friend-requests] 好友申请列表读取成功, uid: {}, count: {}", actor_uid,
              apply_list->size());

    for (const auto& apply_object : *apply_list) {
        auto* apply = payload->add_applications();
        apply->set_from(apply_object->from);
        apply->set_status(apply_object->status);
        apply->set_content(apply_object->content);
    }
    SetTcpPacketStatus(rsp, ErrorCodes::Success);
    co_return rsp;
}

seastar::future<protocol::Packet> friends::pull_friends(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    TcpPacket rsp;
    auto* payload = rsp.mutable_pull_friend_list_response();
    message_io::get_friend_list get_friend_list_operation{actor_uid};
    db::Friend::FriendGroup friend_list = co_await _io.submit(std::move(get_friend_list_operation));
    if (friend_list == nullptr) {
        LOG_DEBUG(logger, "[message/friends] 好友列表为空, uid: {}", actor_uid);
        SetTcpPacketStatus(rsp, ErrorCodes::Success);
        co_return rsp;
    }
    LOG_DEBUG(logger, "[message/friends] 好友列表读取成功, uid: {}, records: {}", actor_uid, friend_list->size());

    for (const auto& friend_object : *friend_list) {
        const int64_t friend_uid = friend_object->uidB;
        message_io::get_user_info get_user_info_operation{friend_uid};
        const db::UserInfo::Ptr friend_info = co_await _io.submit(std::move(get_user_info_operation));
        if (friend_info != nullptr) {
            auto* info = payload->add_friends();
            info->set_uid(friend_uid);
            info->set_name(friend_info->name);
            info->set_age(friend_info->age);
            info->set_sex(friend_info->sex);
            info->set_head_image_url(friend_info->headImageURL);
        }
    }
    SetTcpPacketStatus(rsp, ErrorCodes::Success);
    co_return rsp;
}

} // namespace imm::message_transaction
