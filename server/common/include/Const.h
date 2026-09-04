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
#define PROTOCOL_DATA_SIZE_LEN 4
#define PROTOCOL_HEADER_TOTAL PROTOCOL_DATA_SIZE_LEN

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
