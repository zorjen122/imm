#include "io/options.hh"
#include "GrpcSecurity.h"

namespace wimi::message_io
{
options load_options(const YAML::Node& server)
{
    options value;
    const auto self = server["self"];
    value.node_id = self["name"].as<std::string>();
    value.listener = {self["host"].as<std::string>(), self["rpcPort"].as<uint16_t>()};
    value.shards = self["shards"].as<unsigned>(1);
    if (!value.shards || !value.listener.port ||
        static_cast<uint64_t>(value.listener.port) + value.shards - 1 > 65535) {
        throw std::invalid_argument("invalid Message shard port range");
    }
    const auto security = LoadGrpcSecurityConfig(server);
    value.mtls = security.Mtls();
    value.server_credentials = BuildServerCredentials(security);
    const auto mysql = server["mysql"];
    value.mysql.address = {mysql["host"].as<std::string>(), mysql["classicPort"].as<uint16_t>(3306)};
    value.mysql.user = mysql["user"].as<std::string>();
    value.mysql.password = mysql["password"].as<std::string>();
    value.mysql.schema = mysql["schema"].as<std::string>();
    value.mysql.ca_file = mysql["caFile"].as<std::string>("");
    if (!value.mysql.ca_file.empty()) {
        value.mysql.ca_pem = ReadCredentialFile(value.mysql.ca_file);
    }
    value.file_rpc = {server["file"]["host"].as<std::string>(), server["file"]["rpcPort"].as<uint16_t>()};
    const auto redis = server["redis"];
    value.master_name = redis["masterName"].as<std::string>();
    value.redis_password = redis["password"].as<std::string>("");
    value.sentinel_password = redis["sentinelPassword"].as<std::string>("");
    value.machine_id = redis["machineId"].as<unsigned>();
    for (const auto& address : redis["sentinels"]) {
        value.sentinels.push_back({address["host"].as<std::string>(), address["port"].as<uint16_t>()});
    }
    if (value.sentinels.empty() || value.machine_id > 1023) {
        throw std::invalid_argument("Message requires Redis Sentinel and a 10-bit machine ID");
    }
    if (const auto transaction = server["transaction"]) {
        value.max_requests = transaction["maxRequests"].as<size_t>(value.max_requests);
        value.max_request_bytes = transaction["maxBytes"].as<size_t>(value.max_request_bytes);
    }
    if (const auto queue = server["io"]) {
        value.queue.pending = queue["maxPending"].as<size_t>(value.queue.pending);
        value.queue.bytes = queue["maxBytes"].as<size_t>(value.queue.bytes);
        value.queue.concurrency = queue["concurrency"].as<unsigned>(value.queue.concurrency);
    }
    if (const auto transport = server["transport"]) {
        value.max_writes = transport["maxWrites"].as<size_t>(value.max_writes);
        value.max_write_bytes = transport["maxWriteBytes"].as<size_t>(value.max_write_bytes);
    }
    if (!value.max_requests || !value.max_request_bytes || !value.max_writes || !value.max_write_bytes ||
        !value.queue.pending || !value.queue.bytes || !value.queue.concurrency) {
        throw std::invalid_argument("Message capacities must be positive");
    }
    return value;
}
} // namespace wimi::message_io
