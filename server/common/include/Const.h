#pragma once
#include <functional>

#include <memory>
#include <mutex>
template <typename T> class Singleton
{
  protected:
    Singleton() = default;
    Singleton(const Singleton<T>&) = delete;
    Singleton& operator=(const Singleton<T>& st) = delete;

  public:
    static std::shared_ptr<T> GetInstance()
    {
        static std::shared_ptr<T> instance;
        static std::once_flag s_flag;
        std::call_once(s_flag, [&]() { instance = std::shared_ptr<T>(new T); });
        return instance;
    }
    ~Singleton() {}
};
class Defer
{
  public:
    Defer(std::function<void()> func) : func(func) {}

    ~Defer() { func(); }

  private:
    std::function<void()> func;
};
#include <limits.h>

/* 1.4KB+的缓冲区，指单次读取的最大长度，实际上仅需大于IPv4/IPv6的最大消息单元（MTU）即可
  Pv4 MTU：通常 1500 字节（以太网标准）
  IPv4 最小重组缓冲区：576 字节（RFC 791）
  IPv6 MTU：1280 字节（RFC 8200）
*/

#define PROTOCOL_DATA_MTU 1500
#define PROTOCOL_HEADER_TOTAL 8
#define PROTOCOL_ID_LEN 4
#define PROTOCOL_DATA_SIZE_LEN 4

#define PROTOCOL_RECV_MSS (10 * 1024 * 1024) // 10MB
#define PROTOCOL_SEND_MSS (10 * 1024 * 1024) // 10MB
#define PROTOCOL_QUEUE_MAX_SIZE (10240)

#include <string>
#include <unordered_map>

enum ErrorCodes {
    Success = 0,
    JsonParser = 1000,
    ProtobufPacket,
    RPCFailed,
    VerificationExpired,
    VerificationCodeInvalid,
    UserExist,
    PasswdErr,
    EmailNotMatch,
    PasswdUpFailed,
    PasswdInvalid,
    TokenInvalid,
    UidInvalid,
    UserNotOnline,
    UserNotFriend,
    UserOnline,
    UserOffline,
    NotFound,
    RepeatMessage,
    MysqlFailed,
    FileTypeError,
    InternalError,
    GroupAlreadyExists,
    GroupNotExists,
    GroupNotifyFailed,
    GroupReplyFailed,
    AuthenticationRequired,
    MessageOwnershipInvalid,
    ResourceExhausted = 1028,
    DependencyUnavailable,
    IdempotencyConflict
};

enum ServiceID {
    /* 拉取 */
    ID_PULL_FRIEND_LIST_REQ = 1001, // 拉取好友列表
    ID_PULL_FRIEND_LIST_RSP,

    ID_PULL_FRIEND_APPLY_LIST_REQ, // 拉取好友申请列表
    ID_PULL_FRIEND_APPLY_LIST_RSP,

    ID_PULL_SESSION_MESSAGE_LIST_REQ, // 拉取单个会话消息列表
    ID_PULL_SESSION_MESSAGE_LIST_RSP,

    ID_GROUP_PULL_MEMBER_REQ = 1009, // 拉取群组成员列表
    ID_GROUP_PULL_MEMBER_RSP,

    /* 状态 */
    ID_PING_REQ, // 心跳
    ID_PING_RSP,

    ID_LOGIN_INIT_REQ, // 登录
    ID_LOGIN_INIT_RSP,

    ID_USER_QUIT_REQ, // 登出
    ID_USER_QUIT_RSP,

    ID_INIT_USER_INFO_REQ, // 初始化用户信息
    ID_INIT_USER_INFO_RSP,

    /* 好友 */
    ID_SEARCH_USER_REQ, // 查找用户
    ID_SEARCH_USER_RSP,

    ID_NOTIFY_ADD_FRIEND_REQ, // 发起好友请求
    ID_NOTIFY_ADD_FRIEND_RSP,

    ID_REPLY_ADD_FRIEND_REQ, // 回应好友请求
    ID_REPLY_ADD_FRIEND_RSP,

    ID_REMOVE_FRIEND_REQ, // 删除好友
    ID_REMOVE_FRIEND_RSP,

    /* 消息 */
    ID_TEXT_SEND_REQ, // 发送文本消息
    ID_TEXT_SEND_RSP,

    ID_FILE_SEND_REQ,
    ID_FILE_SEND_RSP,

    ID_FILE_UPLOAD_REQ,
    ID_FILE_UPLOAD_RSP,

    ID_ACK,  // 确认消息
    ID_NULL, // ACK确认的响应包无用，仅因Service响应包将自动发送

    /* 群组 */
    ID_GROUP_CREATE_REQ, // 创建群组
    ID_GROUP_CREATE_RSP,

    ID_GROUP_NOTIFY_JOIN_REQ, // 申请加入群组
    ID_GROUP_NOTIFY_JOIN_RSP,

    ID_GROUP_REPLY_JOIN_REQ, // 处理加入群组申请
    ID_GROUP_REPLY_JOIN_RSP,

    ID_GROUP_QUIT_REQ, // 退出群组
    ID_GROUP_QUIT_RSP,

    ID_GROUP_TEXT_SEND_REQ, // 发送群组消息
    ID_GROUP_TEXT_SEND_RSP,

    ID_TEXT_READ_RECEIPT_NOTIFY, // 单聊文本已读通知
    ID_TEXT_READ_RECEIPT_NOTIFY_RSP,

    ID_FILE_UPLOAD_CHUNK_REQ,
    ID_FILE_UPLOAD_CHUNK_RSP,
    ID_FILE_UPLOAD_COMPLETE_REQ,
    ID_FILE_UPLOAD_COMPLETE_RSP,
    ID_FILE_DOWNLOAD_REQ,
    ID_FILE_DOWNLOAD_RSP,

    ID_GROUP_KICK_MEMBER_REQ,
    ID_GROUP_KICK_MEMBER_RSP,
    ID_GROUP_FILE_SEND_REQ,
    ID_GROUP_FILE_SEND_RSP,

};

static std::unordered_map<int, std::string> errorCodesMap = {{Success, "Success"},
                                                             {JsonParser, "JsonParser"},
                                                             {RPCFailed, "RPCFailed"},
                                                             {VerificationExpired, "VerificationExpired"},
                                                             {VerificationCodeInvalid, "VerificationCodeInvalid"},
                                                             {UserExist, "UserExist"},
                                                             {PasswdErr, "PasswdErr"},
                                                             {EmailNotMatch, "EmailNotMatch"},
                                                             {PasswdUpFailed, "PasswdUpFailed"},
                                                             {PasswdInvalid, "PasswdInvalid"},
                                                             {TokenInvalid, "TokenInvalid"},
                                                             {UidInvalid, "UidInvalid"},
                                                             {UserNotOnline, "UserNotOnline"},
                                                             {UserNotFriend, "UserNotFriend"},
                                                             {UserOnline, "UserOnline"},
                                                             {UserOffline, "UserOffline"},
                                                             {NotFound, "NotFound"},
                                                             {RepeatMessage, "RepeatMessage"},
                                                             {MysqlFailed, "MysqlFailed"},
                                                             {FileTypeError, "FileTypeError"},
                                                             {InternalError, "InternalError"},
                                                             {GroupAlreadyExists, "GroupAlreadyExists"},
                                                             {GroupNotExists, "GroupNotExists"},
                                                             {AuthenticationRequired, "AuthenticationRequired"},
                                                             {MessageOwnershipInvalid, "MessageOwnershipInvalid"},
                                                             {ResourceExhausted, "ResourceExhausted"},
                                                             {DependencyUnavailable, "DependencyUnavailable"},
                                                             {IdempotencyConflict, "IdempotencyConflict"}};

inline bool isRetryableError(int code)
{
    // retryable 是协议契约而非 handler 的临时判断：瞬时依赖/容量故障可用
    // 同一幂等键重试，鉴权、参数和幂等冲突必须停止自动重试。
    switch (code) {
    case RPCFailed:
    case MysqlFailed:
    case InternalError:
    case ResourceExhausted:
    case DependencyUnavailable:
        return true;
    default:
        return false;
    }
}

inline std::string getErrorCodeMsg(int code)
{
    if (errorCodesMap.find(code) != errorCodesMap.end()) {
        return errorCodesMap[code];
    }
    return "";
}
static std::unordered_map<int, std::string> serviceIDMap = {
    {ID_PULL_FRIEND_LIST_REQ, "ID_PULL_FRIEND_LIST_REQ"},
    {ID_PULL_FRIEND_LIST_RSP, "ID_PULL_FRIEND_LIST_RSP"},
    {ID_PULL_FRIEND_APPLY_LIST_REQ, "ID_PULL_FRIEND_APPLY_LIST_REQ"},
    {ID_PULL_FRIEND_APPLY_LIST_RSP, "ID_PULL_FRIEND_APPLY_LIST_RSP"},
    {ID_PULL_SESSION_MESSAGE_LIST_REQ, "ID_PULL_SESSION_MESSAGE_LIST_REQ"},
    {ID_PULL_SESSION_MESSAGE_LIST_RSP, "ID_PULL_SESSION_MESSAGE_LIST_RSP"},
    {ID_GROUP_PULL_MEMBER_REQ, "ID_GROUP_PULL_MEMBER_REQ"},
    {ID_GROUP_PULL_MEMBER_RSP, "ID_GROUP_PULL_MEMBER_RSP"},
    {ID_PING_REQ, "ID_PING_REQ"},
    {ID_PING_RSP, "ID_PING_RSP"},
    {ID_LOGIN_INIT_REQ, "ID_LOGIN_INIT_REQ"},
    {ID_LOGIN_INIT_RSP, "ID_LOGIN_INIT_RSP"},
    {ID_USER_QUIT_REQ, "ID_USER_QUIT_REQ"},
    {ID_USER_QUIT_RSP, "ID_USER_QUIT_RSP"},
    {ID_INIT_USER_INFO_REQ, "ID_INIT_USER_INFO_REQ"},
    {ID_INIT_USER_INFO_RSP, "ID_INIT_USER_INFO_RSP"},
    {ID_SEARCH_USER_REQ, "ID_SEARCH_USER_REQ"},
    {ID_SEARCH_USER_RSP, "ID_SEARCH_USER_RSP"},
    {ID_NOTIFY_ADD_FRIEND_REQ, "ID_NOTIFY_ADD_FRIEND_REQ"},
    {ID_NOTIFY_ADD_FRIEND_RSP, "ID_NOTIFY_ADD_FRIEND_RSP"},
    {ID_REPLY_ADD_FRIEND_REQ, "ID_REPLY_ADD_FRIEND_REQ"},
    {ID_REPLY_ADD_FRIEND_RSP, "ID_REPLY_ADD_FRIEND_RSP"},
    {ID_REMOVE_FRIEND_REQ, "ID_REMOVE_FRIEND_REQ"},
    {ID_REMOVE_FRIEND_RSP, "ID_REMOVE_FRIEND_RSP"},
    {ID_TEXT_SEND_REQ, "ID_TEXT_SEND_REQ"},
    {ID_TEXT_SEND_RSP, "ID_TEXT_SEND_RSP"},
    {ID_FILE_SEND_REQ, "ID_FILE_SEND_REQ"},
    {ID_FILE_SEND_RSP, "ID_FILE_SEND_RSP"},
    {ID_FILE_UPLOAD_REQ, "ID_FILE_UPLOAD_REQ"},
    {ID_FILE_UPLOAD_RSP, "ID_FILE_UPLOAD_RSP"},
    {ID_ACK, "ID_ACK"},
    {ID_NULL, "ID_NULL"},
    {ID_GROUP_CREATE_REQ, "ID_GROUP_CREATE_REQ"},
    {ID_GROUP_CREATE_RSP, "ID_GROUP_CREATE_RSP"},
    {ID_GROUP_NOTIFY_JOIN_REQ, "ID_GROUP_NOTIFY_JOIN_REQ"},
    {ID_GROUP_NOTIFY_JOIN_RSP, "ID_GROUP_NOTIFY_JOIN_RSP"},
    {ID_GROUP_REPLY_JOIN_REQ, "ID_GROUP_REPLY_JOIN_REQ"},
    {ID_GROUP_REPLY_JOIN_RSP, "ID_GROUP_REPLY_JOIN_RSP"},
    {ID_GROUP_QUIT_REQ, "ID_GROUP_QUIT_REQ"},
    {ID_GROUP_QUIT_RSP, "ID_GROUP_QUIT_RSP"},
    {ID_GROUP_TEXT_SEND_REQ, "ID_GROUP_TEXT_SEND_REQ"},
    {ID_GROUP_TEXT_SEND_RSP, "ID_GROUP_TEXT_SEND_RSP"},
    {ID_TEXT_READ_RECEIPT_NOTIFY, "ID_TEXT_READ_RECEIPT_NOTIFY"},
    {ID_TEXT_READ_RECEIPT_NOTIFY_RSP, "ID_TEXT_READ_RECEIPT_NOTIFY_RSP"},
    {ID_FILE_UPLOAD_CHUNK_REQ, "ID_FILE_UPLOAD_CHUNK_REQ"},
    {ID_FILE_UPLOAD_CHUNK_RSP, "ID_FILE_UPLOAD_CHUNK_RSP"},
    {ID_FILE_UPLOAD_COMPLETE_REQ, "ID_FILE_UPLOAD_COMPLETE_REQ"},
    {ID_FILE_UPLOAD_COMPLETE_RSP, "ID_FILE_UPLOAD_COMPLETE_RSP"},
    {ID_FILE_DOWNLOAD_REQ, "ID_FILE_DOWNLOAD_REQ"},
    {ID_FILE_DOWNLOAD_RSP, "ID_FILE_DOWNLOAD_RSP"},
    {ID_GROUP_KICK_MEMBER_REQ, "ID_GROUP_KICK_MEMBER_REQ"},
    {ID_GROUP_KICK_MEMBER_RSP, "ID_GROUP_KICK_MEMBER_RSP"},
    {ID_GROUP_FILE_SEND_REQ, "ID_GROUP_FILE_SEND_REQ"},
    {ID_GROUP_FILE_SEND_RSP, "ID_GROUP_FILE_SEND_RSP"}};

inline std::string getServiceIdString(int id)
{
    if (serviceIDMap.find(id) != serviceIDMap.end()) {
        return serviceIDMap[id];
    }
    return "";
}

inline int __getServiceResponseId(ServiceID id) { return id + 1; }

inline std::string getCurrentDateTime()
{
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);

    static char buffer[80];
    std::tm* now_tm = std::localtime(&now_c);
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", now_tm);
    std::string dateTime(buffer);
    return dateTime;
}
