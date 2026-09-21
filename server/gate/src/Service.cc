#include "Service.h"
#include "StateClient.h"
#include "VerificationService.h"

#include "Const.h"
#include "Logger.h"
#include "Mysql.h"
#include "Redis.h"
#include "spdlog/spdlog.h"
#include <jsoncpp/json/json.h>
#include <jsoncpp/json/value.h>
#include <yaml-cpp/parser.h>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace wimi
{
namespace
{
// Gate 生成短期随机凭证，仅用于把已认证 uid 安全交接给 Chat。
std::string GenerateChatAuthToken()
{
    boost::uuids::random_generator generator;
    return boost::uuids::to_string(generator()) + boost::uuids::to_string(generator());
}

constexpr long kChatAuthTokenTtlSeconds = 15 * 60;
} // namespace

Service::Service()
{
    using namespace std::placeholders;

    OnGetHandle("/test", [this](ServiceResponse& response, Json::Value& requestData) -> bool {
        responseWrite(response, "[TEST]");
        return true;
    });

    OnPostHandle("/post-verifycode", std::bind(&Service::requestVerificationCode, this, _1, _2));
    OnPostHandle("/post-signUp", std::bind(&Service::signUp, this, _1, _2));
    OnPostHandle("/post-forget-password", std::bind(&Service::forgetPassword, this, _1, _2));

    OnPostHandle("/post-signIn", std::bind(&Service::signIn, this, _1, _2));
    OnPostHandle("/post-arrhythmia", std::bind(&Service::chatArrhythmia, this, _1, _2));
}

void Service::responseWrite(ServiceResponse& response, const std::string& data) { response.body += data; }

void Service::OnGetHandle(std::string url, HttpHandler handler)
{
    auto ret = getHandlers.insert(make_pair(url, handler));
    if (ret.second == false)
        logger->error("RegisterGet insert is wrong!");
}

void Service::OnPostHandle(std::string url, HttpHandler handler)
{
    auto ret = postHandlers.insert(make_pair(url, handler));
    if (ret.second == false)
        logger->error("RegisterPost insert is wrong!");
}

Service::~Service() {}

ServiceResponse Service::Handle(std::string_view body, const std::string& path, std::string_view method)
{
    ServiceResponse response;
    const auto& handlers = method == "GET" ? getHandlers : postHandlers;
    auto found = handlers.find(path);
    if ((method != "GET" && method != "POST") || found == handlers.end()) {
        response.status = 404;
        response.contentType = "text/plain";
        response.body = "url not found";
        return response;
    }

    Json::Value source;
    Json::Reader reader;
    const bool parsed = reader.parse(body.data(), body.data() + body.size(), source);
    if (method == "POST" && (!parsed || source.empty())) {
        Json::Value error;
        error["error"] = ErrorCodes::JsonParser;
        response.body = "content-type not support | " + error.toStyledString();
        return response;
    }

    found->second(response, source);
    return response;
}

bool Service::requestVerificationCode(ServiceResponse& response, Json::Value& requestData)
{
    Json::Value rspInfo;
    Defer _([&]() { responseWrite(response, rspInfo.toStyledString()); });

    if (!requestData.isMember("email")) {
        logger->info("[post_verifycode-email] Failed to parse JSON data!");

        rspInfo["error"] = ErrorCodes::JsonParser;
        return false;
    }

    auto email = VerificationService::NormalizeEmail(requestData["email"].asString());
    logger->info("service-post_verifycode] email as {}", email);

    auto result = VerificationService::GetInstance()->RequestEmailCode(email);
    rspInfo["error"] = result.error;
    rspInfo["email"] = email;
    rspInfo["issued"] = result.issued;
    if (!result.developmentCode.empty())
        rspInfo["verificationCode"] = result.developmentCode;
    return result.error == ErrorCodes::Success;
}

bool Service::signUp(ServiceResponse& response, Json::Value& requestData)
{
    Json::Value rspInfo;
    Defer _([&]() { responseWrite(response, rspInfo.toStyledString()); });

    if (!requestData.isMember("username") || !requestData.isMember("password") || !requestData.isMember("email") ||
        !requestData.isMember("verifycode")) {
        rspInfo["error"] = ErrorCodes::JsonParser;
        return false;
    }

    auto username = requestData["username"].asString();
    auto password = requestData["password"].asString();
    auto email = VerificationService::NormalizeEmail(requestData["email"].asString());
    auto verificationCode = requestData["verifycode"].asString();
    if (username.empty() || password.empty() || !VerificationService::IsValidEmail(email)) {
        rspInfo["error"] = ErrorCodes::JsonParser;
        return false;
    }
    logger->info("[signUp]: start, username: {}, email: {}", username, email);

    int hasEmail = db::MysqlDao::GetInstance()->hasEmail(email);
    if (hasEmail != 0) {
        logger->info("[signUp]: email is exist");
        rspInfo["error"] = ErrorCodes::UserExist;
        return false;
    }

    int hasUsername = db::MysqlDao::GetInstance()->hasUsername(username);
    if (hasUsername != 0) {
        logger->info("[signUp]: username is exist");
        rspInfo["error"] = ErrorCodes::UserExist;
        return false;
    }

    if (!VerificationService::GetInstance()->VerifyAndConsume(email, verificationCode)) {
        logger->info("[signUp]: invalid or expired verification code");
        rspInfo["error"] = ErrorCodes::VerificationCodeInvalid;
        return false;
    }
    auto uid = db::RedisDao::GetInstance()->generateUserId();

    db::User::Ptr user(new db::User(0, uid, username, password, email));
    int hasUser = db::MysqlDao::GetInstance()->userRegister(user);
    if (hasUser != 0) {
        logger->info("[signUp]: user or email exist");
        rspInfo["error"] = -1;
        return false;
    }

    logger->info("[signUp]: success! uid as {}, username as {}, email as {}", uid, username, email);
    rspInfo["error"] = 0;
    rspInfo["uid"] = Json::Value::Int64(uid);
    rspInfo["username"] = username;
    rspInfo["email"] = email;

    return true;
}

bool Service::signIn(ServiceResponse& response, Json::Value& requestData)
{
    Json::Value rspInfo;
    Defer _([&]() { responseWrite(response, rspInfo.toStyledString()); });

    auto username = requestData["username"].asString();
    auto password = requestData["password"].asString();
    logger->info("[signIn]: start, username: {}", username);

    auto user = wimi::db::MysqlDao::GetInstance()->getUser(username);
    if (user == nullptr) {
        logger->debug("user is not existd!");
        rspInfo["error"] = -1;
        return false;
    }

    if (user->password != password) {
        logger->debug("password or email input is wrong!");
        rspInfo["error"] = -1;
        return false;
    }

    auto node = rpc::StateClient::GetInstance()->PickConnectionGateway(user->uid);
    if (node.empty()) {
        logger->info("rpc request state service is fialed, user id as {} ", user->uid);
        rspInfo["error"] = -1;
        return false;
    }

    auto hasFirstSignIn = db::MysqlDao::GetInstance()->hasUserInfo(user->uid);
    if (hasFirstSignIn == false)
        rspInfo["init"] = 1;
    else
        rspInfo["init"] = 0;
    logger->info("[sigIn] user: {} login success", user->username);
    rspInfo["error"] = 0;
    rspInfo["uid"] = Json::Value::Int64(user->uid);
    rspInfo["ip"] = node.ip;
    rspInfo["port"] = node.port;
    rspInfo["gatewayId"] = node.nodeId;

    // token 按 uid 覆盖并设置 TTL；持久化失败时不得返回可用登录结果。
    std::string chatAuthToken = GenerateChatAuthToken();
    if (!db::RedisDao::GetInstance()->setChatAuthToken(user->uid, chatAuthToken, kChatAuthTokenTtlSeconds)) {
        logger->warn("[signIn] failed to persist chat auth token, uid: {}", user->uid);
        rspInfo.clear();
        rspInfo["error"] = ErrorCodes::InternalError;
        return false;
    }
    rspInfo["chatToken"] = chatAuthToken;
    rspInfo["chatTokenExpiresIn"] = Json::Value::Int64(kChatAuthTokenTtlSeconds);

    return true;
}

bool Service::chatArrhythmia(ServiceResponse& response, Json::Value& requestData)
{
    Json::Value rspInfo;
    Defer _([&]() { responseWrite(response, rspInfo.toStyledString()); });

    auto uid = requestData["uid"].asInt();
    // Mysql select...
    rpc::ServerNode node = rpc::StateClient::GetInstance()->PickConnectionGateway(uid);
    if (node.ip.empty() || node.port == 0) {
        logger->error("[chat_arrhythmia] get im server failed");
        rspInfo["error"] = ErrorCodes::RPCFailed;
        return false;
    }

    rspInfo["error"] = ErrorCodes::Success;
    rspInfo["host"] = node.ip;
    rspInfo["port"] = node.port;

    return true;
}

bool Service::forgetPassword(ServiceResponse& response, Json::Value& requestData)
{
    Json::Value rspInfo;
    Defer _([&]() { responseWrite(response, rspInfo.toStyledString()); });

    if (!requestData.isMember("username") || !requestData.isMember("password") || !requestData.isMember("email") ||
        !requestData.isMember("verifycode")) {
        rspInfo["error"] = ErrorCodes::JsonParser;
        return false;
    }

    auto username = requestData["username"].asString();
    auto password = requestData["password"].asString();
    auto email = VerificationService::NormalizeEmail(requestData["email"].asString());
    auto verificationCode = requestData["verifycode"].asString();

    auto user = db::MysqlDao::GetInstance()->getUser(username);
    if (user == nullptr) {
        logger->info("user does not exist");
        rspInfo["error"] = ErrorCodes::NotFound;
        return false;
    }
    if (VerificationService::NormalizeEmail(user->email) != email) {
        rspInfo["error"] = ErrorCodes::EmailNotMatch;
        return false;
    }
    if (password.empty()) {
        rspInfo["error"] = ErrorCodes::PasswdInvalid;
        return false;
    }
    if (!VerificationService::GetInstance()->VerifyAndConsume(email, verificationCode)) {
        logger->info("verification code is expired or invalid");
        rspInfo["error"] = ErrorCodes::VerificationCodeInvalid;
        return false;
    }

    user->password = password;
    int updateResult = db::MysqlDao::GetInstance()->userModifyPassword(user);
    if (updateResult != 0) {
        logger->error(" update password is failed");
        rspInfo["error"] = ErrorCodes::PasswdUpFailed;
        return false;
    }

    logger->info("password updated for username: {}", username);

    rspInfo["error"] = 0;
    rspInfo["username"] = username;

    return true;
}
}; // namespace wimi
