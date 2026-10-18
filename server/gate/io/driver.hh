#pragma once
#include "io/operations.hh"
#include "io/grpc_client.hh"
#include "io/options.hh"
#include "io/redis_connection.hh"
#include "io/mysql_connection.hh"
#include "io/smtp_connection.hh"
#include "io/dns_resolver.hh"
#include <set>

namespace wimi::gate_io
{
// 统一 I/O 队列的后端：将操作映射到协议调用，向业务返回拥有自身存储的结果。
// 协议读写及其回调归各适配器负责，本层管理连接作用域、停止取消和操作级组合。
class driver
{
    options _options;
    dns_resolver _dns;
    seastar::shared_ptr<seastar::tls::certificate_credentials> _smtp_credentials;
    std::unique_ptr<grpc_client> _grpc;
    // 集合仅登记在途连接以便 abort()，不拥有这些指针；连接由具名操作协程的
    // 局部变量持有，跨挂起点存活，完成异步 close 后才由 defer 注销。
    std::set<redis_connection*> _redis;
    std::set<mysql_connection*> _mysql;
    std::set<smtp_connection*> _smtp;
    std::optional<seastar::future<>> _dns_stopped;
    bool _stopping{};
    bool _mysql_initialized{};
    seastar::future<endpoint> discover_master();
    seastar::future<redis_reply> redis_command(std::vector<std::string> arguments);
    seastar::future<mysql_result> mysql_query(std::string sql, std::vector<mysql_parameter> parameters);

  public:
    explicit driver(options config);
    seastar::future<> start();
    void abort();
    seastar::future<> stop();
    seastar::future<bool> execute(issue_verification_code);
    seastar::future<std::string> execute(get_verification_code);
    seastar::future<bool> execute(clear_verification_code);
    seastar::future<> execute(send_verification_email);
    seastar::future<std::optional<user_account>> execute(find_user);
    seastar::future<bool> execute(has_user_info);
    seastar::future<gateway_node> execute(pick_gateway);
    seastar::future<> execute(set_chat_token);
    seastar::future<bool> execute(consume_verification_code);
    seastar::future<int64_t> execute(generate_user_id);
    seastar::future<bool> execute(user_exists);
    seastar::future<bool> execute(register_user);
    seastar::future<> execute(update_password);
};
} // namespace wimi::gate_io
