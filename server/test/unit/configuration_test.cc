#include "configuration.hh"
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void rejected(const YAML::Node& base, const YAML::Node& extra, const std::string& id)
{
    bool failed{};
    try {
        imm::configuration::select(base, extra, id);
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "invalid topology accepted");
}
} // namespace

int main()
{
    const auto base = YAML::Load(R"(
shared:
  mysql: {port: 3306}
  redis: {masterName: test}
nodes:
  gate: {service: gate, host: localhost, port: 18080, shards: 2}
  state: {service: state, host: localhost, port: 50052}
  file: {service: file, host: localhost, port: 51100}
  gateway-1: {service: gateway, host: localhost, port: 8090, shards: 2}
  message-1: {service: message, host: localhost, port: 50055, machineId: 1}
)");
    const auto extra = YAML::Load(R"(
nodes:
  gateway-2: {service: gateway, host: localhost, port: 8091}
  message-2: {service: message, host: localhost, port: 50056, machineId: 2}
)");
    auto single = imm::configuration::select(base, {}, "state");
    auto cluster = imm::configuration::select(base, extra, "state");
    require(single["gateways"].size() == 1 && single["messages"].size() == 1, "single-node topology mismatch");
    require(cluster["gateways"].size() == 2 && cluster["messages"].size() == 2, "cluster topology mismatch");
    const auto gateway = imm::configuration::select(base, extra, "gateway-2");
    require(gateway["self"]["name"].as<std::string>() == "gateway-2" &&
                gateway["self"]["port"].as<unsigned>() == 8091 && gateway["stateRPC"]["port"].as<unsigned>() == 50052,
            "node/dependency selection mismatch");
    auto message = imm::configuration::select(base, extra, "message-2");
    require(message["file"]["port"].as<unsigned>() == 51100 && message["redis"]["machineId"].as<unsigned>() == 2,
            "Message dependency/identity mismatch");
    message["mysql"]["port"] = 999;
    require(base["shared"]["mysql"]["port"].as<unsigned>() == 3306, "selected node mutated shared configuration");
    for (const auto* id : {"gate", "gateway-1"}) {
        require(imm::configuration::select(base, {}, id)["self"]["shards"].as<unsigned>() == 2,
                "node shard count not preserved");
        auto invalid = YAML::Clone(base);
        invalid["nodes"][id]["shards"] = 0;
        rejected(invalid, {}, id);
    }

    rejected(base, extra, "absent");
    rejected(base, YAML::Load("nodes: {gateway-1: {}}"), "state");
    rejected(base, YAML::Load("nodes: {}\nshared: {}"), "state");
    rejected(base, YAML::Load("nodes: {state-2: {service: state, host: localhost, port: 50053}}"), "state");
    for (const auto port : {0u, 65536u, 4294967295u}) {
        auto invalid = YAML::Clone(base);
        invalid["nodes"]["message-1"]["port"] = port;
        rejected(invalid, {}, "message-1");
    }
    auto invalid = YAML::Clone(base);
    invalid["nodes"]["message-1"]["port"] = 65535;
    invalid["nodes"]["message-1"]["shards"] = 2;
    rejected(invalid, {}, "state");
    std::cout << "PASS topology selection, additive nodes, dependency resolution, isolation and invalid inputs\n";
}
