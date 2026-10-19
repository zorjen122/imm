#pragma once
#include "gateway_message.pb.h"
#include <seastar/core/future.hh>
#include <optional>
#include <variant>

namespace wimi::gateway_io
{
struct validate_token {
    using result_type = bool;
    int64_t uid;
    std::string token;
    size_t byte_size() const { return token.size(); }
};
struct prepare_profile {
    using result_type = bool;
    protocol::LoginRequest login;
    size_t byte_size() const { return login.ByteSizeLong(); }
};
struct register_device {
    using result_type = bool;
    int64_t uid;
    std::string device_id;
    size_t byte_size() const { return device_id.size(); }
};
struct bound_session {
    gateway::SessionLease current;
    std::optional<gateway::SessionLease> previous;
};
struct bind_session {
    using result_type = bound_session;
    int64_t uid;
    gateway::SessionLease route;
    size_t byte_size() const { return route.ByteSizeLong(); }
};
struct refresh_session {
    using result_type = bool;
    int64_t uid;
    gateway::SessionLease route;
    size_t byte_size() const { return route.ByteSizeLong(); }
};
struct clear_session {
    using result_type = void;
    int64_t uid;
    gateway::SessionLease route;
    size_t byte_size() const { return route.ByteSizeLong(); }
};
// 发送操作只负责准入及排队，消费者不会为等待远端业务结果长期占用执行槽。
// 业务拿到 ticket 后自行等待 result，在途额度由 driver 的 pending 表持有。
struct command_ticket {
    seastar::future<gateway::CommandResult> result;
};
struct send_command {
    using result_type = command_ticket;
    gateway::CommandEnvelope command;
    size_t byte_size() const { return command.ByteSizeLong(); }
};
using io_operation = std::variant<validate_token, prepare_profile, register_device, bind_session, refresh_session,
                                  clear_session, send_command>;
} // namespace wimi::gateway_io
