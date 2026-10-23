#pragma once
#include "reactor/types.hh"
#include "state.pb.h"
#include <yaml-cpp/yaml.h>
#include <cstddef>
#include <string>
#include <vector>

namespace imm::state_service
{
struct gateway_node {
    std::string id;
    std::string host;
    uint16_t port{};
    bool active{};
};
struct options {
    io::endpoint listener;
    std::size_t max_requests{512};
    std::vector<gateway_node> gateways;
    ::state::MessageTopology messages;
};
// 配置只在启动线程解析；各 shard 收到独立的普通值与 protobuf 快照。
options load_options(const YAML::Node& config);
} // namespace imm::state_service
