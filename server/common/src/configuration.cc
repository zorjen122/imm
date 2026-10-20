#include "configuration.hh"
#include "Logger.h"
#include <array>
#include <cstdlib>
#include <cstdint>
#include <set>
#include <stdexcept>

namespace imm::configuration
{
namespace
{
YAML::Node current;
void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::invalid_argument(message);
    }
}
} // namespace
YAML::Node select(const YAML::Node& base, const YAML::Node& extra, const std::string& node_id)
{
    require(base.IsMap() && base["nodes"].IsMap(), "configuration requires a nodes map");
    auto nodes = YAML::Clone(base["nodes"]);
    if (extra.IsDefined() && !extra.IsNull()) {
        require(extra.IsMap() && extra.size() == 1 && extra["nodes"].IsMap(),
                "extra configuration may only append nodes");
        for (const auto& item : extra["nodes"]) {
            const auto id = item.first.as<std::string>();
            require(!nodes[id], "duplicate node: " + id);
            nodes[id] = YAML::Clone(item.second);
        }
    }
    std::set<std::string> singleton;
    YAML::Node state;
    YAML::Node file;
    YAML::Node gateways(YAML::NodeType::Sequence);
    YAML::Node messages(YAML::NodeType::Sequence);
    for (const auto& item : nodes) {
        const auto id = item.first.as<std::string>();
        const auto node = item.second;
        const auto service = node["service"].as<std::string>();
        require(service == "gate" || service == "file" || service == "state" || service == "gateway" ||
                    service == "message",
                "unknown service: " + service);
        const auto port = node["port"].as<unsigned>();
        const auto shards = node["shards"].as<unsigned>(1);
        require(port && port <= 65535 && shards &&
                    (service != "message" || static_cast<uint64_t>(port) + shards - 1 <= 65535),
                "invalid node port/shards: " + id);
        require(!node["host"].as<std::string>().empty(), "empty node host: " + id);
        if (service == "gate" || service == "file" || service == "state") {
            require(singleton.insert(service).second, "duplicate singleton service: " + service);
        }
        auto endpoint = YAML::Clone(node);
        endpoint["name"] = id;
        if (service == "state") {
            state = endpoint;
        } else if (service == "file") {
            file = endpoint;
        } else if (service == "gateway") {
            gateways.push_back(endpoint);
        } else if (service == "message") {
            messages.push_back(endpoint);
        }
    }
    require(!node_id.empty() && nodes[node_id].IsDefined(), "unknown IMM_NODE: " + node_id);
    auto result = YAML::Clone(nodes[node_id]);
    auto self = YAML::Clone(result);
    self["name"] = node_id;
    if (self["listenHost"]) {
        self["host"] = self["listenHost"];
    }
    result["self"] = self;
    const auto shared = base["shared"];
    for (const auto* key : std::array{"mysql", "redis", "transportSecurity"}) {
        if (!result[key] && shared && shared[key]) {
            result[key] = YAML::Clone(shared[key]);
        }
    }
    if (result["redis"]) {
        result["redis"]["machineId"] = self["machineId"].as<unsigned>(1);
    }
    const auto service = self["service"].as<std::string>();
    if (service == "gate" || service == "gateway") {
        require(state.IsMap(), "State node is required");
        result["stateRPC"] = state;
    }
    if (service == "message") {
        require(file.IsMap(), "File node is required");
        result["file"] = file;
    }
    result["gateways"] = gateways;
    result["messages"] = messages;
    return result;
}
bool load(const std::string& path)
{
    try {
        const char* node = std::getenv("IMM_NODE");
        require(node && *node, "IMM_NODE is required");
        YAML::Node extra;
        if (const char* configured = std::getenv("IMM_EXTRA_CONFIG"); configured && *configured) {
            extra = YAML::LoadFile(configured);
        }
        current = select(YAML::LoadFile(path), extra, node);
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR(logger, "configuration failed: {}", error.what());
        return false;
    }
}
YAML::Node server() { return current; }
} // namespace imm::configuration
