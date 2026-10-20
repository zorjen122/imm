#include "io/options.hh"
#include <cstdlib>
#include <stdexcept>

namespace imm::gate_io
{
namespace
{
std::string environment(const char* name, std::string value)
{
    if (const char* configured = std::getenv(name)) {
        return configured;
    }
    return value;
}
bool environment_bool(const char* name, bool value)
{
    const auto configured = environment(name, value ? "true" : "false");
    if (configured == "1" || configured == "true" || configured == "on") {
        return true;
    }
    if (configured == "0" || configured == "false" || configured == "off") {
        return false;
    }
    throw std::invalid_argument(std::string("invalid boolean: ") + name);
}
} // namespace
options load_options(const YAML::Node& server)
{
    options result;
    const auto mysql = server["mysql"];
    result.mysql.address = {mysql["host"].as<std::string>(), mysql["port"].as<uint16_t>(3306)};
    result.mysql.user = mysql["user"].as<std::string>();
    result.mysql.password = mysql["password"].as<std::string>();
    result.mysql.schema = mysql["schema"].as<std::string>();
    result.mysql.ca_file = mysql["caFile"].as<std::string>("");
    result.state_rpc = {server["stateRPC"]["host"].as<std::string>(), server["stateRPC"]["port"].as<uint16_t>()};
    const auto redis = server["redis"];
    result.machine_id = redis["machineId"].as<unsigned>();
    if (result.machine_id > 1023) {
        throw std::invalid_argument("Redis machineId must be at most 1023");
    }
    result.master_name = redis["masterName"].as<std::string>();
    result.redis_password = redis["password"].as<std::string>("");
    result.sentinel_password = redis["sentinelPassword"].as<std::string>("");
    for (const auto& node : redis["sentinels"]) {
        result.sentinels.push_back({node["host"].as<std::string>(), node["port"].as<uint16_t>()});
    }
    if (result.sentinels.empty()) {
        throw std::invalid_argument("at least one Redis Sentinel is required");
    }
    const auto verification = server["verification"];
    if (verification) {
        result.max_attempts = verification["maxAttempts"].as<int>(5);
        result.code_ttl_seconds = verification["codeTtlSeconds"].as<int>(180);
        result.cooldown_seconds = verification["resendCooldownSeconds"].as<int>(60);
        result.expose_code = verification["exposeCodeInResponse"].as<bool>(false);
    }
    std::string url = "smtps://smtp.qq.com:465";
    const auto email = verification ? verification["email"] : YAML::Node();
    if (email) {
        result.email_enabled = email["enabled"].as<bool>(false);
        result.smtp_user = email["username"].as<std::string>("");
        result.smtp_password = email["password"].as<std::string>("");
        result.smtp_ca_file = email["caFile"].as<std::string>("");
        url = email["smtpUrl"].as<std::string>(url);
    }
    result.email_enabled = environment_bool("IMM_VERIFY_SEND_EMAIL", result.email_enabled);
    result.expose_code = environment_bool("IMM_VERIFY_EXPOSE_CODE", result.expose_code);
    result.smtp_user = environment("IMM_VERIFY_EMAIL_USER", std::move(result.smtp_user));
    result.smtp_password = environment("IMM_VERIFY_EMAIL_PASS", std::move(result.smtp_password));
    url = environment("IMM_VERIFY_SMTP_URL", std::move(url));
    result.implicit_tls = url.starts_with("smtps://");
    if (!result.implicit_tls && !url.starts_with("smtp://")) {
        throw std::invalid_argument("SMTP URL must use smtp:// or smtps://");
    }
    const auto authority = url.substr(result.implicit_tls ? 8 : 7);
    const auto colon = authority.rfind(':');
    result.smtp.host = colon == std::string::npos ? authority : authority.substr(0, colon);
    const auto port =
        colon == std::string::npos ? (result.implicit_tls ? 465 : 587) : std::stoul(authority.substr(colon + 1));
    if (result.smtp.host.empty() || port == 0 || port > 65535 ||
        authority.find_first_of("/\r\n@") != std::string::npos) {
        throw std::invalid_argument("invalid SMTP authority");
    }
    result.smtp.port = static_cast<uint16_t>(port);
    if (result.max_attempts <= 0 || result.code_ttl_seconds <= 0 || result.cooldown_seconds <= 0) {
        throw std::invalid_argument("verification TTL and cooldown must be positive");
    }
    const auto queue = server["io"];
    if (queue) {
        result.queue.pending = queue["maxPending"].as<size_t>(result.queue.pending);
        result.queue.bytes = queue["maxBytes"].as<size_t>(result.queue.bytes);
        result.queue.concurrency = queue["concurrency"].as<unsigned>(result.queue.concurrency);
    }
    const auto transaction = server["transaction"];
    if (transaction) {
        result.max_requests = transaction["maxRequests"].as<size_t>(result.max_requests);
        result.max_request_bytes = transaction["maxBytes"].as<size_t>(result.max_request_bytes);
    }
    if (!result.max_requests || !result.max_request_bytes) {
        throw std::invalid_argument("transaction admission limits must be positive");
    }
    return result;
}
} // namespace imm::gate_io
