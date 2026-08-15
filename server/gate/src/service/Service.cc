#include "Service.h"
#include "StateClient.h"
#include "VerificationService.h"

#include "Const.h"
#include "HttpSession.h"
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

    OnGetHandle("/test", [this](HttpSession::ResponsePtr response, Json::Value& requestData) -> bool {
        responseWrite(response, "[TEST]");
        return true;
    });

    OnPostHandle("/post-verifycode", std::bind(&Service::requestVerificationCode, this, _1, _2));
    OnPostHandle("/post-signUp", std::bind(&Service::signUp, this, _1, _2));
    OnPostHandle("/post-forget-password", std::bind(&Service::forgetPassword, this, _1, _2));

    OnPostHandle("/post-signIn", std::bind(&Service::signIn, this, _1, _2));
    OnPostHandle("/post-arrhythmia", std::bind(&Service::chatArrhythmia, this, _1, _2));
}

void Service::responseWrite(HttpSession::ResponsePtr response, const std::string& data)
{
    beast::ostream(response->body()) << data;
}

Json::Value Service::parseRequest(std::shared_ptr<HttpSession> connection)
{
    auto buffer = connection->GetRequest()->body().data();
    auto body = boost::beast::buffers_to_string(buffer);

    Json::Reader reader{};
    Json::Value src{};
    bool parseSuccess = reader.parse(body, src);

    return parseSuccess ? src : Json::Value();
}

void Service::OnGetHandle(std::string url, HttpHandler handler)
{
    auto ret = getHandlers.insert(make_pair(url, handler));
    if (ret.second == false)
        businessLogger->error("RegisterGet insert is wrong!");
}

void Service::OnPostHandle(std::string url, HttpHandler handler)
{
    auto ret = postHandlers.insert(make_pair(url, handler));
    if (ret.second == false)
        businessLogger->error("RegisterPost insert is wrong!");
}

Service::~Service() {}

bool Service::Handle(std::shared_ptr<HttpSession> connection, std::string path, http::verb method)
{
    HttpHandler handler;
    bool hasFound = false;
    if (method == http::verb::get) {
        if (getHandlers.find(path) != getHandlers.end()) {
            handler = getHandlers[path];
            hasFound = true;
        }
    } else if (method == http::verb::post) {
        if (postHandlers.find(path) != postHandlers.end()) {
            handler = postHandlers[path];
            hasFound = true;
        }
    }

    auto response = connection->GetResponse();
    if (hasFound == false || handler == nullptr) {
        response->result(http::status::not_found);
        response->set(http::field::content_type, "text/plain");
        responseWrite(response, "url not found");
        return false;
    }

    connection->GetResponse()->set(http::field::content_type, "application/json");
    Json::Value source = parseRequest(connection);

    if (source.empty() && method == http::verb::post) {
        Json::Value rsp;
        rsp["error"] = ErrorCodes::JsonParser;
        responseWrite(response, "content-type not support | " + rsp.toStyledString());
        return true;
    }

    businessLogger->info("[service-handle] path as {}", path);

    response->result(http::status::ok);
    response->set(http::field::server, "GateServer");
    bool handleSuccess = handler(response, source);

    return handleSuccess;
}

bool Service::requestVerificationCode(HttpSession::ResponsePtr response, Json::Value& requestData)
{
    Json::Value rspInfo;
    Defer _([&]() { responseWrite(response, rspInfo.toStyledString()); });

    if (!requestData.isMember("email")) {
        businessLogger->info("[post_verifycode-email] Failed to parse JSON data!");

        rspInfo["error"] = ErrorCodes::JsonParser;
        return false;
    }

    auto email = VerificationService::NormalizeEmail(requestData["email"].asString());
    businessLogger->info("service-post_verifycode] email as {}", email);

    auto result = VerificationService::GetInstance()->RequestEmailCode(email);
    rspInfo["error"] = result.error;
    rspInfo["email"] = email;
    rspInfo["issued"] = result.issued;
    if (!result.developmentCode.empty())
        rspInfo["verificationCode"] = result.developmentCode;
    return result.error == ErrorCodes::Success;
}

bool Service::signUp(HttpSession::ResponsePtr response, Json::Value& requestData)
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
    businessLogger->info("[signUp]: start, username: {}, email: {}", username, email);

    int hasEmail = db::MysqlDao::GetInstance()->hasEmail(email);
    if (hasEmail != 0) {
        businessLogger->info("[signUp]: email is exist");
        rspInfo["error"] = ErrorCodes::UserExist;
        return false;
    }

    int hasUsername = db::MysqlDao::GetInstance()->hasUsername(username);
    if (hasUsername != 0) {
        businessLogger->info("[signUp]: username is exist");
        rspInfo["error"] = ErrorCodes::UserExist;
        return false;
    }

    if (!VerificationService::GetInstance()->VerifyAndConsume(email, verificationCode)) {
        businessLogger->info("[signUp]: invalid or expired verification code");
        rspInfo["error"] = ErrorCodes::VerificationCodeInvalid;
        return false;
    }
    auto uid = db::RedisDao::GetInstance()->generateUserId();

    db::User::Ptr user(new db::User(0, uid, username, password, email));
    int hasUser = db::MysqlDao::GetInstance()->userRegister(user);
    if (hasUser != 0) {
        businessLogger->info("[signUp]: user or email exist");
        rspInfo["error"] = -1;
        return false;
    }

    businessLogger->info("[signUp]: success! uid as {}, username as {}, email as {}", uid, username, email);
    rspInfo["error"] = 0;
    rspInfo["uid"] = Json::Value::Int64(uid);
    rspInfo["username"] = username;
    rspInfo["email"] = email;

    return true;
}

bool Service::signIn(HttpSession::ResponsePtr response, Json::Value& requestData)
{
    Json::Value rspInfo;
    Defer _([&]() { responseWrite(response, rspInfo.toStyledString()); });

    auto username = requestData["username"].asString();
    auto password = requestData["password"].asString();
    businessLogger->info("[signIn]: start, username: {}", username);

    auto user = wimi::db::MysqlDao::GetInstance()->getUser(username);
    if (user == nullptr) {
        businessLogger->debug("user is not existd!");
        rspInfo["error"] = -1;
        return false;
    }

    if (user->password != password) {
        businessLogger->debug("password or email input is wrong!");
        rspInfo["error"] = -1;
        return false;
    }

    auto node = rpc::StateClient::GetInstance()->PickConnectionGateway(user->uid);
    if (node.empty()) {
        businessLogger->info("rpc request state service is fialed, user id as {} ", user->uid);
        rspInfo["error"] = -1;
        return false;
    }

    auto hasFirstSignIn = db::MysqlDao::GetInstance()->hasUserInfo(user->uid);
    if (hasFirstSignIn == false)
        rspInfo["init"] = 1;
    else
        rspInfo["init"] = 0;
    businessLogger->info("[sigIn] user: {} login success", user->username);
    rspInfo["error"] = 0;
    rspInfo["uid"] = Json::Value::Int64(user->uid);
    rspInfo["ip"] = node.ip;
    rspInfo["port"] = node.port;
    rspInfo["gatewayId"] = node.nodeId;

    // token 按 uid 覆盖并设置 TTL；持久化失败时不得返回可用登录结果。
    std::string chatAuthToken = GenerateChatAuthToken();
    if (!db::RedisDao::GetInstance()->setChatAuthToken(user->uid, chatAuthToken, kChatAuthTokenTtlSeconds)) {
        businessLogger->warn("[signIn] failed to persist chat auth token, uid: {}", user->uid);
        rspInfo.clear();
        rspInfo["error"] = ErrorCodes::InternalError;
        return false;
    }
    rspInfo["chatToken"] = chatAuthToken;
    rspInfo["chatTokenExpiresIn"] = Json::Value::Int64(kChatAuthTokenTtlSeconds);

    return true;
}

bool Service::chatArrhythmia(HttpSession::ResponsePtr response, Json::Value& requestData)
{
    Json::Value rspInfo;
    Defer _([&]() { responseWrite(response, rspInfo.toStyledString()); });

    auto uid = requestData["uid"].asInt();
    // Mysql select...
    rpc::ServerNode node = rpc::StateClient::GetInstance()->PickConnectionGateway(uid);
    if (node.ip.empty() || node.port == 0) {
        businessLogger->error("[chat_arrhythmia] get im server failed");
        rspInfo["error"] = ErrorCodes::RPCFailed;
        return false;
    }

    rspInfo["error"] = ErrorCodes::Success;
    rspInfo["host"] = node.ip;
    rspInfo["port"] = node.port;

    return true;
}

bool Service::forgetPassword(HttpSession::ResponsePtr response, Json::Value& requestData)
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
        businessLogger->info("user does not exist");
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
        businessLogger->info("verification code is expired or invalid");
        rspInfo["error"] = ErrorCodes::VerificationCodeInvalid;
        return false;
    }

    user->password = password;
    int updateResult = db::MysqlDao::GetInstance()->userModifyPassword(user);
    if (updateResult != 0) {
        businessLogger->error(" update password is failed");
        rspInfo["error"] = ErrorCodes::PasswdUpFailed;
        return false;
    }

    businessLogger->info("password updated for username: {}", username);

    rspInfo["error"] = 0;
    rspInfo["username"] = username;

    return true;
}
}; // namespace wimi
