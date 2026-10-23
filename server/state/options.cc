#include "options.hh"
#include <stdexcept>

namespace imm::state_service
{
options load_options(const YAML::Node& config)
{
    options result;
    const auto transaction = config["transaction"];
    const auto max_requests = transaction ? transaction["maxRequests"].as<int64_t>(512) : 512;
    if (max_requests <= 0) {
        throw std::invalid_argument("State transaction.maxRequests must be positive");
    }
    result.max_requests = static_cast<std::size_t>(max_requests);
    result.listener = {config["self"]["host"].as<std::string>(), config["self"]["port"].as<uint16_t>()};
    for (const auto& source : config["gateways"]) {
        gateway_node node{source["name"].as<std::string>(), source["host"].as<std::string>(),
                          source["port"].as<uint16_t>(), source["status"].as<std::string>("active") == "active"};
        node.active = node.active && !node.host.empty() && node.port != 0;
        result.gateways.push_back(std::move(node));
    }
    for (const auto& source : config["messages"]) {
        const auto host = source["host"].as<std::string>();
        const auto port = source["port"].as<uint16_t>();
        const auto status = source["status"].as<std::string>("active");
        if (status != "active" || host.empty() || port == 0) {
            continue;
        }
        auto* node = result.messages.add_nodes();
        node->set_node_id(source["name"].as<std::string>());
        node->set_host(host);
        node->set_port(port);
        node->set_status(status);
        node->set_weight(source["weight"].as<unsigned>(1));
        node->set_shard_count(source["shards"].as<unsigned>(1));
    }
    return result;
}
} // namespace imm::state_service
