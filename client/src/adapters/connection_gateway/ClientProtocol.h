#pragma once

#include "tcp_message.qpb.h"

#include <QtGlobal>

namespace wimi::client::protocol
{

using Operation = wimi::protocol::OperationGadget::Operation;

enum ErrorCode : int {
    Success = 0,
    JsonParser = 1000,
    ProtobufPacket = 1001,
    RpcFailed = 1002,
    VerifyExpired = 1003,
    VerifyCodeError = 1004,
    UserExists = 1005,
    PasswordError = 1006,
    EmailMismatch = 1007,
    PasswordUpdateFailed = 1008,
    PasswordInvalid = 1009,
    TokenInvalid = 1010,
    UidInvalid = 1011,
    UserNotOnline = 1012,
    UserNotFriend = 1013,
    UserOnline = 1014,
    UserOffline = 1015,
    NotFound = 1016,
    RepeatMessage = 1017,
    MysqlFailed = 1018,
    FileTypeError = 1019,
    InternalError = 1020,
    GroupAlreadyExists = 1021,
    GroupNotExists = 1022,
    GroupNotifyFailed = 1023,
    GroupReplyFailed = 1024,
    AuthenticationRequired = 1025,
    MessageOwnershipInvalid = 1026,
    DeadlineExceeded = 1027,
    ResourceExhausted = 1028,
    DependencyUnavailable = 1029,
    IdempotencyConflict = 1030,
};

inline wimi::protocol::PushTypeGadget::PushType PushTypeFor(const wimi::protocol::Packet& packet)
{
    using PushType = wimi::protocol::PushTypeGadget::PushType;
    if (packet.hasTextMessagePush())
        return PushType::PUSH_TYPE_TEXT_MESSAGE;
    if (packet.hasFileMessagePush())
        return PushType::PUSH_TYPE_FILE_MESSAGE;
    if (packet.hasFriendRequestPush())
        return PushType::PUSH_TYPE_FRIEND_REQUEST;
    if (packet.hasFriendReplyPush())
        return PushType::PUSH_TYPE_FRIEND_REPLY;
    if (packet.hasGroupJoinRequestPush())
        return PushType::PUSH_TYPE_GROUP_JOIN_REQUEST;
    if (packet.hasGroupChangePush())
        return PushType::PUSH_TYPE_GROUP_CHANGE;
    if (packet.hasReadReceiptPush())
        return PushType::PUSH_TYPE_READ_RECEIPT;
    return PushType::PUSH_TYPE_UNSPECIFIED;
}

constexpr bool IsRetryableError(int code)
{
    return code == RpcFailed || code == MysqlFailed || code == InternalError || code == DeadlineExceeded ||
           code == ResourceExhausted || code == DependencyUnavailable;
}

} // namespace wimi::client::protocol
