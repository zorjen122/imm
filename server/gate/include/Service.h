#pragma once
#include <functional>
#include <jsoncpp/json/value.h>
#include <map>
#include <memory>

#include "Const.h"
#include <string>
#include <string_view>

namespace wimi
{

struct ServiceResponse {
    unsigned status{200};
    std::string contentType{"application/json"};
    std::string body;
};

using HttpHandler = std::function<bool(ServiceResponse&, Json::Value&)>;

class Service : public Singleton<Service>
{
    friend class Singleton<Service>;

  public:
    ~Service();
    ServiceResponse Handle(std::string_view body, const std::string& path, std::string_view method);
    void OnGetHandle(std::string, HttpHandler);
    void OnPostHandle(std::string, HttpHandler);

  private:
    bool requestVerificationCode(ServiceResponse&, Json::Value&);
    bool signUp(ServiceResponse&, Json::Value&);
    bool signIn(ServiceResponse&, Json::Value&);
    bool forgetPassword(ServiceResponse&, Json::Value&);
    bool chatArrhythmia(ServiceResponse&, Json::Value&);

    void responseWrite(ServiceResponse&, const std::string&);

  private:
    Service();
    std::map<std::string, HttpHandler> postHandlers;
    std::map<std::string, HttpHandler> getHandlers;
};
}; // namespace wimi
