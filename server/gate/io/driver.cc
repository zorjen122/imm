#include "io/driver.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/file.hh>
#include <charconv>
#include <cassert>
#include <seastar/core/sleep.hh>

namespace wimi::gate_io
{
namespace
{
std::string verification_prefix(const std::string& email) { return "im:verify:{" + email + "}:"; }
void check_reply(bool condition)
{
    if (!condition) {
        throw std::runtime_error("unexpected Redis reply");
    }
}
} // namespace
driver::driver(options config) : _options(std::move(config)) {}
seastar::future<> driver::start()
{
    if (mysql_thread_init() != 0) {
        throw std::runtime_error("MySQL shard initialization failed");
    }
    _mysql_initialized = true;
    if (!_options.mysql.ca_file.empty()) {
        const auto pem = co_await seastar::util::read_entire_file_contiguous(_options.mysql.ca_file);
        _options.mysql.ca_pem.assign(pem.data(), pem.size());
    }
    if (_options.email_enabled) {
        if (_options.smtp_user.empty() || _options.smtp_password.empty()) {
            throw std::invalid_argument("SMTP credentials are required when email is enabled");
        }
        _smtp_credentials = seastar::make_shared<seastar::tls::certificate_credentials>();
        if (_options.smtp_ca_file.empty()) {
            co_await _smtp_credentials->set_system_trust();
        } else {
            co_await _smtp_credentials->set_x509_trust_file(_options.smtp_ca_file, seastar::tls::x509_crt_format::PEM);
        }
    }
    _grpc = std::make_unique<grpc_client>(_options.state_rpc.host + ":" + std::to_string(_options.state_rpc.port));
}
seastar::future<endpoint> driver::discover_master()
{
    std::exception_ptr last;
    for (const endpoint& address : _options.sentinels) {
        if (_stopping) {
            throw io::stopped();
        }
        redis_connection connection;
        _redis.insert(&connection);
        auto remove = seastar::defer([this, &connection]() noexcept { _redis.erase(&connection); });
        std::optional<endpoint> result;
        try {
            const auto ip = co_await _dns.resolve_name(address.host, seastar::net::inet_address::family::INET);
            if (_stopping) {
                throw io::stopped();
            }
            co_await connection.connect(fmt::format("{}", ip), address.port);
            if (!_options.sentinel_password.empty()) {
                std::vector<std::string> auth{"AUTH", _options.sentinel_password};
                co_await connection.command(std::move(auth));
            }
            std::vector<std::string> command{"SENTINEL", "get-master-addr-by-name", _options.master_name};
            auto reply = co_await connection.command(std::move(command));
            check_reply(reply.type == REDIS_REPLY_ARRAY && reply.elements.size() == 2);
            const auto port = std::stoul(reply.elements[1]);
            check_reply(port > 0 && port <= 65535);
            result = endpoint{std::move(reply.elements[0]), static_cast<uint16_t>(port)};
        } catch (...) {
            last = std::current_exception();
        }
        co_await connection.close();
        if (result) {
            co_return std::move(*result);
        }
    }
    std::rethrow_exception(last);
}
seastar::future<redis_reply> driver::redis_command(std::vector<std::string> arguments)
{
    if (_stopping) {
        throw io::stopped();
    }
    const auto address = co_await discover_master();
    redis_connection connection;
    _redis.insert(&connection);
    auto remove = seastar::defer([this, &connection]() noexcept { _redis.erase(&connection); });
    std::optional<redis_reply> result;
    std::exception_ptr failure;
    try {
        const auto ip = co_await _dns.resolve_name(address.host, seastar::net::inet_address::family::INET);
        if (_stopping) {
            throw io::stopped();
        }
        co_await connection.connect(fmt::format("{}", ip), address.port);
        if (!_options.redis_password.empty()) {
            std::vector<std::string> auth{"AUTH", _options.redis_password};
            co_await connection.command(std::move(auth));
        }
        result = co_await connection.command(std::move(arguments));
    } catch (...) {
        failure = std::current_exception();
    }
    co_await connection.close();
    if (failure) {
        std::rethrow_exception(failure);
    }
    co_return std::move(*result);
}
seastar::future<bool> driver::execute(issue_verification_code operation)
{
    const auto prefix = verification_prefix(operation.email);
    std::vector<std::string> command{"EVAL",
                                     R"(
if redis.call('EXISTS', KEYS[2]) == 1 then return 0 end
redis.call('SETEX', KEYS[1], ARGV[2], ARGV[1])
redis.call('SETEX', KEYS[2], ARGV[3], '1')
redis.call('DEL', KEYS[3])
return 1
)",
                                     "3",
                                     prefix + "code",
                                     prefix + "cooldown",
                                     prefix + "attempts",
                                     operation.code,
                                     std::to_string(operation.ttl_seconds),
                                     std::to_string(operation.cooldown_seconds)};
    auto result = co_await redis_command(std::move(command));
    check_reply(result.type == REDIS_REPLY_INTEGER && (result.integer == 0 || result.integer == 1));
    co_return result.integer == 1;
}
seastar::future<std::string> driver::execute(get_verification_code operation)
{
    std::vector<std::string> command{"GET", verification_prefix(operation.email) + "code"};
    auto result = co_await redis_command(std::move(command));
    check_reply(result.type == REDIS_REPLY_STRING || result.type == REDIS_REPLY_NIL);
    co_return std::move(result.text);
}
seastar::future<bool> driver::execute(clear_verification_code operation)
{
    const auto prefix = verification_prefix(operation.email);
    std::vector<std::string> command{"EVAL",        R"(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('DEL', KEYS[1], KEYS[2], KEYS[3])
return 1
)", "3", prefix + "code", prefix + "attempts", prefix + "cooldown",
                                     operation.code};
    auto result = co_await redis_command(std::move(command));
    check_reply(result.type == REDIS_REPLY_INTEGER);
    co_return result.integer == 1;
}
seastar::future<> driver::execute(send_verification_email operation)
{
    if (_stopping) {
        throw io::stopped();
    }
    smtp_connection connection(_options, _smtp_credentials);
    _smtp.insert(&connection);
    auto remove = seastar::defer([this, &connection]() noexcept { _smtp.erase(&connection); });
    std::exception_ptr failure;
    try {
        const auto ip = co_await _dns.resolve_name(_options.smtp.host, seastar::net::inet_address::family::INET);
        if (_stopping) {
            throw io::stopped();
        }
        co_await connection.send(ip, _options, std::move(operation), _smtp_credentials);
    } catch (...) {
        failure = std::current_exception();
        connection.abort();
    }
    co_await connection.close();
    if (failure) {
        std::rethrow_exception(failure);
    }
}
seastar::future<mysql_result> driver::mysql_query(std::string sql, std::vector<mysql_parameter> parameters)
{
    if (_stopping) {
        throw io::stopped();
    }
    mysql_connection connection;
    _mysql.insert(&connection);
    auto remove = seastar::defer([this, &connection]() noexcept { _mysql.erase(&connection); });
    mysql_result result;
    std::exception_ptr failure;
    try {
        const auto ip =
            co_await _dns.resolve_name(_options.mysql.address.host, seastar::net::inet_address::family::INET);
        if (_stopping) {
            throw io::stopped();
        }
        co_await connection.connect(fmt::format("{}", ip), _options.mysql);
        result = co_await connection.query(std::move(sql), std::move(parameters));
    } catch (...) {
        failure = std::current_exception();
        connection.abort();
    }
    co_await connection.close();
    if (failure) {
        std::rethrow_exception(failure);
    }
    co_return result;
}
seastar::future<std::optional<user_account>> driver::execute(find_user operation)
{
    std::vector<mysql_parameter> parameters;
    parameters.emplace_back(std::move(operation.username));
    auto query = co_await mysql_query("SELECT uid, username, password, email FROM users WHERE username = ? LIMIT 1",
                                      std::move(parameters));
    auto& result = query.row;
    if (result.empty()) {
        co_return std::nullopt;
    }
    if (result.size() != 4) {
        throw std::runtime_error("invalid user query");
    }
    const auto uid = std::stoll(result[0]);
    co_return user_account{uid, std::move(result[1]), std::move(result[2]), std::move(result[3])};
}
seastar::future<bool> driver::execute(has_user_info operation)
{
    std::vector<mysql_parameter> parameters;
    parameters.emplace_back(operation.uid);
    auto result = co_await mysql_query("SELECT 1 FROM userInfo WHERE uid = ? LIMIT 1", std::move(parameters));
    co_return !result.row.empty();
}
seastar::future<gateway_node> driver::execute(pick_gateway operation)
{
    if (_stopping) {
        throw io::stopped();
    }
    co_return co_await _grpc->pick_gateway(operation.uid);
}
seastar::future<> driver::execute(set_chat_token operation)
{
    std::vector<std::string> command{"SETEX", "im:chatAuth:" + std::to_string(operation.uid),
                                     std::to_string(operation.ttl_seconds), std::move(operation.token)};
    auto result = co_await redis_command(std::move(command));
    check_reply(result.type == REDIS_REPLY_STATUS && result.text == "OK");
}
seastar::future<bool> driver::execute(consume_verification_code operation)
{
    const auto prefix = verification_prefix(operation.email);
    std::vector<std::string> command{"EVAL",
                                     R"(
local current = redis.call('GET', KEYS[1])
if not current then return 0 end
if current == ARGV[1] then
  redis.call('DEL', KEYS[1], KEYS[2], KEYS[3])
  return 1
end
local attempts = redis.call('INCR', KEYS[2])
local ttl = redis.call('TTL', KEYS[1])
if attempts == 1 and ttl > 0 then redis.call('EXPIRE', KEYS[2], ttl) end
if attempts >= tonumber(ARGV[2]) then redis.call('DEL', KEYS[1], KEYS[2]) end
return 0
)",
                                     "3",
                                     prefix + "code",
                                     prefix + "attempts",
                                     prefix + "cooldown",
                                     std::move(operation.code),
                                     std::to_string(operation.max_attempts)};
    const auto reply = co_await redis_command(std::move(command));
    check_reply(reply.type == REDIS_REPLY_INTEGER && (reply.integer == 0 || reply.integer == 1));
    co_return reply.integer == 1;
}
seastar::future<int64_t> driver::execute(generate_user_id operation)
{
    // Preserve the existing shared Redis sequence and epoch. Sequence exhaustion
    // suspends this consumer; it must never sleep the reactor thread.
    constexpr int64_t epoch = 1672531200000;
    while (!_stopping) {
        const auto timestamp =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count() -
            epoch;
        std::vector<std::string> command{"INCR", "im:userId:" + std::to_string(timestamp)};
        const auto reply = co_await redis_command(std::move(command));
        check_reply(reply.type == REDIS_REPLY_INTEGER && reply.integer > 0);
        if (reply.integer <= 4095) {
            co_return (timestamp << 22) | (static_cast<int64_t>(operation.machine_id) << 12) | reply.integer;
        }
        co_await seastar::sleep(std::chrono::milliseconds(1));
    }
    throw io::stopped();
}
seastar::future<bool> driver::execute(user_exists operation)
{
    std::vector<mysql_parameter> parameters;
    parameters.emplace_back(std::move(operation.username));
    parameters.emplace_back(std::move(operation.email));
    const auto reply =
        co_await mysql_query("SELECT 1 FROM users WHERE username = ? OR email = ? LIMIT 1", std::move(parameters));
    co_return !reply.row.empty();
}
seastar::future<bool> driver::execute(register_user operation)
{
    std::vector<mysql_parameter> parameters;
    parameters.emplace_back(operation.uid);
    parameters.emplace_back(std::move(operation.username));
    parameters.emplace_back(std::move(operation.password));
    parameters.emplace_back(std::move(operation.email));
    const auto reply =
        co_await mysql_query("INSERT INTO users(uid, username, password, email, createTime) VALUES(?, ?, ?, ?, NOW())",
                             std::move(parameters));
    co_return reply.error != 1062;
}
seastar::future<> driver::execute(update_password operation)
{
    std::vector<mysql_parameter> parameters;
    parameters.emplace_back(std::move(operation.password));
    parameters.emplace_back(operation.uid);
    co_await mysql_query("UPDATE users SET password = ? WHERE uid = ?", std::move(parameters));
}

void driver::abort()
{
    _stopping = true;
    _dns_stopped.emplace(_dns.close());
    if (_grpc) {
        _grpc->abort();
    }
    for (redis_connection* connection : _redis) {
        connection->abort();
    }
    for (mysql_connection* connection : _mysql) {
        connection->abort();
    }
    for (smtp_connection* connection : _smtp) {
        connection->abort();
    }
}
seastar::future<> driver::stop()
{
    assert(_redis.empty() && _mysql.empty() && _smtp.empty());
    if (_dns_stopped) {
        co_await std::move(*_dns_stopped);
        _dns_stopped.reset();
    }
    if (_grpc) {
        co_await _grpc->close();
        _grpc.reset();
    }
    _smtp_credentials = nullptr;
    if (_mysql_initialized) {
        mysql_thread_end();
        _mysql_initialized = false;
    }
}
} // namespace wimi::gate_io
