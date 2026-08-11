#pragma once

#include <QtGlobal>

namespace wimi::client::protocol
{

enum ServiceId : quint32 {
    PullFriendListRequest = 1001,
    PullFriendListResponse = 1002,
    PullFriendApplyListRequest = 1003,
    PullFriendApplyListResponse = 1004,
    PullSessionMessagesRequest = 1005,
    PullSessionMessagesResponse = 1006,
    PullGroupMembersRequest = 1009,
    PullGroupMembersResponse = 1010,
    PingRequest = 1011,
    PingResponse = 1012,
    LoginRequest = 1013,
    LoginResponse = 1014,
    QuitRequest = 1015,
    QuitResponse = 1016,
    InitUserInfoRequest = 1017,
    InitUserInfoResponse = 1018,
    SearchUserRequest = 1019,
    SearchUserResponse = 1020,
    AddFriendRequest = 1021,
    AddFriendResponse = 1022,
    ReplyFriendRequest = 1023,
    ReplyFriendResponse = 1024,
    RemoveFriendRequest = 1025,
    RemoveFriendResponse = 1026,
    SendTextRequest = 1027,
    SendTextResponse = 1028,
    SendFileRequest = 1029,
    SendFileResponse = 1030,
    UploadFileRequest = 1031,
    UploadFileResponse = 1032,
    Ack = 1033,
    NullResponse = 1034,
    CreateGroupRequest = 1035,
    CreateGroupResponse = 1036,
    JoinGroupRequest = 1037,
    JoinGroupResponse = 1038,
    ReplyJoinGroupRequest = 1039,
    ReplyJoinGroupResponse = 1040,
    QuitGroupRequest = 1041,
    QuitGroupResponse = 1042,
    SendGroupTextRequest = 1043,
    SendGroupTextResponse = 1044,
    TextReadReceiptNotification = 1045,
    TextReadReceiptNotificationResponse = 1046,
    FileUploadChunkRequest = 1047,
    FileUploadChunkResponse = 1048,
    FileUploadCompleteRequest = 1049,
    FileUploadCompleteResponse = 1050,
    FileDownloadRequest = 1051,
    FileDownloadResponse = 1052,
    KickGroupMemberRequest = 1053,
    KickGroupMemberResponse = 1054,
    SendGroupFileRequest = 1055,
    SendGroupFileResponse = 1056,
};

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

constexpr quint32 ResponseFor(quint32 requestServiceId)
{
    return requestServiceId == Ack ? NullResponse : requestServiceId + 1;
}

constexpr bool IsRetryableError(int code)
{
    return code == RpcFailed || code == MysqlFailed || code == InternalError || code == DeadlineExceeded ||
           code == ResourceExhausted || code == DependencyUnavailable;
}

} // namespace wimi::client::protocol
