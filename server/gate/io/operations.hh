#pragma once
#include <string>
#include <optional>
#include <cstdint>
#include <variant>

namespace imm::gate_io
{
struct gateway_node {
    std::string host;
    uint16_t port{};
    std::string id;
};
struct issue_verification_code {
    using result_type = bool;
    std::string email;
    std::string code;
    int ttl_seconds;
    int cooldown_seconds;
    size_t byte_size() const { return email.size() + code.size(); }
};
struct get_verification_code {
    using result_type = std::string;
    std::string email;
    size_t byte_size() const { return email.size(); }
};
struct clear_verification_code {
    using result_type = bool;
    std::string email;
    std::string code;
    size_t byte_size() const { return email.size() + code.size(); }
};
struct send_verification_email {
    using result_type = void;
    std::string recipient;
    std::string code;
    int ttl_seconds;
    size_t byte_size() const { return recipient.size() + code.size(); }
};
struct user_account {
    int64_t uid{};
    std::string username;
    std::string password;
    std::string email;
};
struct find_user {
    using result_type = std::optional<user_account>;
    std::string username;
    size_t byte_size() const { return username.size(); }
};
struct has_user_info {
    using result_type = bool;
    int64_t uid;
    size_t byte_size() const { return 0; }
};
struct pick_gateway {
    using result_type = gateway_node;
    int64_t uid;
    size_t byte_size() const { return 0; }
};
struct set_chat_token {
    using result_type = void;
    int64_t uid;
    std::string token;
    int ttl_seconds;
    size_t byte_size() const { return token.size(); }
};
struct consume_verification_code {
    using result_type = bool;
    std::string email;
    std::string code;
    int max_attempts;
    size_t byte_size() const { return email.size() + code.size(); }
};
struct generate_user_id {
    using result_type = int64_t;
    unsigned machine_id;
    size_t byte_size() const { return 0; }
};
struct user_exists {
    using result_type = bool;
    std::string username;
    std::string email;
    size_t byte_size() const { return username.size() + email.size(); }
};
struct register_user {
    using result_type = bool;
    int64_t uid;
    std::string username;
    std::string password;
    std::string email;
    size_t byte_size() const { return username.size() + password.size() + email.size(); }
};
struct update_password {
    using result_type = void;
    int64_t uid;
    std::string password;
    size_t byte_size() const { return password.size(); }
};
// I/O 操作的静态注册表，与 HTTP 请求 message 分层：这里只描述一次依赖操作的参数和结果。
// 每项声明 result_type、动态参数大小及对应的 driver::execute 重载；
// 统一队列在编译期检查类型匹配，业务只通过 submit(操作对象) 提交。
using io_operation =
    std::variant<issue_verification_code, get_verification_code, clear_verification_code, send_verification_email,
                 find_user, has_user_info, pick_gateway, set_chat_token, consume_verification_code, generate_user_id,
                 user_exists, register_user, update_password>;
} // namespace imm::gate_io
