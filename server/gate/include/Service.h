#pragma once
#include <boost/beast/http/verb.hpp>
#include <functional>
#include <jsoncpp/json/value.h>
#include <map>
#include <memory>

#include "Const.h"
#include "HttpSession.h"
#include <boost/beast.hpp>

namespace wimi
{

namespace http = boost::beast::http;

typedef std::function<bool(HttpSession::ResponsePtr, Json::Value&)> HttpHandler;

class Service : public Singleton<Service>
{
    friend class Singleton<Service>;

  public:
    ~Service();
    bool Handle(std::shared_ptr<HttpSession> connection, std::string path, http::verb method);
    void OnGetHandle(std::string, HttpHandler);
    void OnPostHandle(std::string, HttpHandler);

  private:
    bool requestVerificationCode(HttpSession::ResponsePtr, Json::Value&);
    bool signUp(HttpSession::ResponsePtr, Json::Value&);
    bool signIn(HttpSession::ResponsePtr, Json::Value&);
    bool forgetPassword(HttpSession::ResponsePtr, Json::Value&);
    bool chatArrhythmia(HttpSession::ResponsePtr, Json::Value&);

    void responseWrite(HttpSession::ResponsePtr, const std::string&);
    Json::Value parseRequest(std::shared_ptr<HttpSession> connection);

  private:
    Service();
    std::map<std::string, HttpHandler> postHandlers;
    std::map<std::string, HttpHandler> getHandlers;
};
}; // namespace wimi
