#pragma once
#include "IoQueue.h"
#include <yaml-cpp/yaml.h>
#include <cstdint>
#include <string>
#include <vector>

namespace wimi::gate_io
{
struct endpoint {
    std::string host;
    uint16_t port;
};
struct mysql_options {
    endpoint address{"127.0.0.1", 3306};
    std::string user;
    std::string password;
    std::string schema;
    std::string ca_file;
    std::string ca_pem;
};
struct options {
    io::queue_limits queue;
    size_t max_requests{128};
    size_t max_request_bytes{16 * 1024 * 1024};
    int max_attempts{5};
    unsigned machine_id{};
    mysql_options mysql;
    endpoint state_rpc{"127.0.0.1", 50052};
    std::vector<endpoint> sentinels;
    std::string master_name;
    std::string redis_password;
    std::string sentinel_password;
    int code_ttl_seconds{180};
    int cooldown_seconds{60};
    bool email_enabled{};
    bool expose_code{};
    endpoint smtp{"smtp.qq.com", 465};
    bool implicit_tls{true};
    std::string smtp_user;
    std::string smtp_password;
    std::string smtp_ca_file;
};
// Reads immutable startup configuration. Environment overrides retain the
// existing WIMI_VERIFY_* meanings; no configuration file is read per request.
options load_options(const YAML::Node& server);
} // namespace wimi::gate_io
