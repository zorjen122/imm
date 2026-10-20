#pragma once
#include "IoQueue.h"
#include "reactor/types.hh"
#include <grpcpp/security/server_credentials.h>
#include <yaml-cpp/yaml.h>
#include <memory>

namespace imm::message_io
{
struct options {
    std::string node_id;
    io::endpoint listener;
    unsigned shards{1};
    bool mtls{};
    std::shared_ptr<grpc::ServerCredentials> server_credentials;
    io::mysql_options mysql;
    io::endpoint file_rpc;
    std::vector<io::endpoint> sentinels;
    std::string master_name;
    std::string redis_password;
    std::string sentinel_password;
    unsigned machine_id{};
    io::queue_limits queue{512, 64 * 1024 * 1024, 16};
    size_t max_requests{512};
    size_t max_request_bytes{64 * 1024 * 1024};
    size_t max_writes{32768};
    size_t max_write_bytes{256 * 1024 * 1024};
};
// 在开始接收业务前加载配置和凭据；各 shard 只复用已载入的内容。
options load_options(const YAML::Node& server);
} // namespace imm::message_io
