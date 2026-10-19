#pragma once
#include "IoQueue.h"
#include "reactor/types.hh"
#include <grpcpp/security/credentials.h>
#include <yaml-cpp/yaml.h>
#include <vector>
#include <memory>

namespace wimi::gateway_io
{
struct options {
    std::string gateway_id;
    std::string instance_id;
    uint16_t port{};
    unsigned command_rate_limit{100};
    io::mysql_options mysql;
    io::endpoint state_rpc;
    std::vector<io::endpoint> sentinels;
    std::string master_name;
    std::string redis_password;
    std::string sentinel_password;
    std::shared_ptr<grpc::ChannelCredentials> message_credentials;
    io::queue_limits queue{512, 64 * 1024 * 1024, 16};
    size_t max_requests{512};
    size_t max_request_bytes{64 * 1024 * 1024};
    size_t max_writes{65536};
    size_t max_write_bytes{512 * 1024 * 1024};
    size_t max_pending{32768};
    size_t max_pending_bytes{256 * 1024 * 1024};
};
options load_options(const YAML::Node& server);
} // namespace wimi::gateway_io
