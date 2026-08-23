#include "service.h"
#include "Configer.h"
#include "global.h"
#include "spdlog/spdlog.h"
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <algorithm>
#include <cstdint>
#include <string>

namespace wimi::rpc
{
namespace
{

std::vector<ServiceNodeInfo> LoadNodes(const YAML::Node& server, const std::string& sectionName,
                                       const std::string& prefix, const std::string& totalKey,
                                       const std::string& portKey)
{
    std::vector<ServiceNodeInfo> nodes;
    auto section = server[sectionName];
    if (!section || !section[totalKey])
        return nodes;

    auto total = section[totalKey].as<int>();
    nodes.reserve(std::max(total, 0));
    for (int i = 1; i <= total; ++i) {
        auto source = section[prefix + std::to_string(i)];
        if (!source)
            continue;

        ServiceNodeInfo node;
        node.id = source["name"].as<std::string>();
        node.host = source["host"].as<std::string>();
        node.port = source[portKey].as<unsigned short>();
        node.status = source["status"] ? source["status"].as<std::string>() : std::string{"active"};
        node.weight = source["weight"] ? source["weight"].as<unsigned int>() : 1;
        nodes.push_back(std::move(node));
    }
    return nodes;
}

} // namespace

StateServiceImpl::StateServiceImpl()
{
    auto conf = Configer::getNode("server");

    gatewayNodes = LoadNodes(conf, "connection-gateway", "g", "gateway-total", "port");
    messageNodes = LoadNodes(conf, "message", "m", "message-total", "streamPort");
}

grpc::Status StateServiceImpl::PickConnectionGateway(grpc::ServerContext*, const ConnectUser*, ConnectUserRsp* response)
{
    if (gatewayNodes.empty())
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no connection gateway configured");

    const std::size_t start = gatewayRouteCount.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t offset = 0; offset < gatewayNodes.size(); ++offset) {
        const auto& node = gatewayNodes[(start + offset) % gatewayNodes.size()];
        if (!node.active())
            continue;
        response->set_ip(node.host);
        response->set_port(node.port);
        response->set_node_id(node.id);
        return grpc::Status::OK;
    }

    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no active connection gateway");
}

grpc::Status StateServiceImpl::ListMessageNodes(grpc::ServerContext*, const TopologyRequest*, MessageTopology* response)
{
    for (const auto& node : messageNodes) {
        if (!node.active())
            continue;
        auto* target = response->add_nodes();
        target->set_node_id(node.id);
        target->set_host(node.host);
        target->set_port(node.port);
        target->set_status(node.status);
        target->set_weight(node.weight);
    }
    return grpc::Status::OK;
}
}; // namespace wimi::rpc
