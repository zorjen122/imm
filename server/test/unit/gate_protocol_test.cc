#include "api/protocol.hh"
#include "Const.h"
#include <cstdlib>
#include <iostream>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace
int main()
{
    using namespace imm;
    gate_api::verification_request verification;
    require(gate_protocol::decode(R"({"email":"  Alice@Example.TEST "})", verification) == 0 &&
                verification.email() == "alice@example.test",
            "email decoding/normalization failed");
    for (const auto* body : {"{}", "null", "[]", "{", R"({"email":null})", R"({"email":3})",
                             R"({"email":"a@example.test","unknown":1})"}) {
        require(gate_protocol::decode(body, verification) != 0, "invalid verification payload accepted");
    }
    require(gate_protocol::decode(R"({"email":"invalid"})", verification) == ErrorCodes::EmailNotMatch,
            "email content validation missing");
    gate_api::sign_in_request login;
    require(gate_protocol::decode(R"({"username":"alice","password":"password"})", login) == 0,
            "login decoding failed");
    for (const auto* body :
         {R"({"username":"alice"})", R"({"username":"","password":"p"})", R"({"username":"alice","password":false})"}) {
        require(gate_protocol::decode(body, login) != 0, "invalid credentials accepted");
    }
    gate_api::gateway_request gateway;
    require(gate_protocol::decode(R"({"uid":"9007199254740993"})", gateway) == 0 && gateway.uid() == 9007199254740993LL,
            "64-bit UID precision lost");
    for (const auto* body : {"{}", R"({"uid":null})", R"({"uid":0})", R"({"uid":-1})", R"({"uid":1.5})",
                             R"({"uid":"9223372036854775808"})"}) {
        require(gate_protocol::decode(body, gateway) != 0, "invalid UID accepted");
    }
    gate_api::sign_up_request signup;
    require(gate_protocol::decode(
                R"({"username":"new","password":"p","email":"NEW@example.test","verifycode":"123456"})", signup) == 0 &&
                signup.email() == "new@example.test",
            "signup normalization failed");
    require(
        gate_protocol::decode(R"({"username":"new","password":"p","email":"new@example.test","verifycode":"12a456"})",
                              signup) == ErrorCodes::VerificationCodeInvalid,
        "non-digit verification code accepted");
    gate_api::reset_password_request reset;
    require(gate_protocol::decode(R"({"username":"new","password":"p","email":"new@example.test"})", reset) != 0,
            "reset without verification code accepted");
    gate_api::sign_in_response response;
    response.set_error(0);
    response.set_uid(9007199254740993LL);
    response.set_chat_token("token");
    const auto encoded = gate_protocol::encode(response);
    require(encoded.find("\"error\":0") != std::string::npos &&
                encoded.find("\"uid\":\"9007199254740993\"") != std::string::npos &&
                encoded.find("\"chatToken\":\"token\"") != std::string::npos,
            "ProtoJSON presence, naming or int64 encoding violated");
    require(std::holds_alternative<gate_protocol::immediate_handler>(gate_protocol::routes().front().handler),
            "health route must use immediate processing");
}
