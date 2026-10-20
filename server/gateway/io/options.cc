#include "io/options.hh"
#include "GrpcSecurity.h"
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace wimi::gateway_io
{
options load_options(const YAML::Node& server)
{
    options value;
    value.gateway_id = server["self"]["name"].as<std::string>();
    value.port = server["self"]["port"].as<uint16_t>();
    value.command_rate_limit = server["self"]["commandRateLimit"].as<unsigned>(100);
    value.instance_id = boost::uuids::to_string(boost::uuids::random_generator{}());
    const auto mysql = server["mysql"];
    value.mysql.address = {mysql["host"].as<std::string>(), mysql["classicPort"].as<uint16_t>(3306)};
    value.mysql.user = mysql["user"].as<std::string>();
    value.mysql.password = mysql["password"].as<std::string>();
    value.mysql.schema = mysql["schema"].as<std::string>();
    value.mysql.ca_file = mysql["caFile"].as<std::string>("");
    value.state_rpc = {server["stateRPC"]["host"].as<std::string>(), server["stateRPC"]["port"].as<uint16_t>()};
    const auto redis = server["redis"];
    value.master_name = redis["masterName"].as<std::string>();
    value.redis_password = redis["password"].as<std::string>("");
    value.sentinel_password = redis["sentinelPassword"].as<std::string>("");
    for (const auto& address : redis["sentinels"]) {
        value.sentinels.push_back({address["host"].as<std::string>(), address["port"].as<uint16_t>()});
    }
    if (value.sentinels.empty()) {
        throw std::invalid_argument("Redis Sentinel is required");
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
        value.max_pending = transport["maxPending"].as<size_t>(value.max_pending);
        value.max_pending_bytes = transport["maxPendingBytes"].as<size_t>(value.max_pending_bytes);
    }
    if (!value.max_requests || !value.max_request_bytes || !value.max_writes || !value.max_write_bytes ||
        !value.max_pending || !value.max_pending_bytes || !value.queue.pending || !value.queue.bytes ||
        !value.queue.concurrency) {
        throw std::invalid_argument("Gateway capacities must be positive");
    }
    // initialize_grpc() 必须先执行。凭据文件仅在开始接收请求前读取，后续重连复用内存凭据。
    value.message_credentials = BuildChannelCredentials(LoadGrpcSecurityConfig(server));
    return value;
}
} // namespace wimi::gateway_io
