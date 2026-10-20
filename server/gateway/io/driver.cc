#include "io/driver.hh"
#include "io/message_link.hh"
#include "message_route.hh"
#include "Logger.h"
#include "TcpMessageCodec.h"
#include <google/protobuf/util/json_util.h>
#include <grpc/impl/channel_arg_names.h>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/util/file.hh>
#include <seastar/util/defer.hh>
#include <sys/syscall.h>
#include <unistd.h>
#include <cassert>
#include <algorithm>

namespace imm::gateway_io
{
using io::endpoint;
using io::mysql_connection;
using io::mysql_parameter;
using io::mysql_result;
using io::redis_connection;
using io::redis_reply;
namespace
{
void check_reply(bool value)
{
    if (!value) {
        throw std::runtime_error("invalid Redis response");
    }
}
std::string lease_key(int64_t uid, const std::string& device)
{
    return "im:session:" + std::to_string(uid) + ":" + device;
}
gateway::SessionLease decode_lease(const std::string& encoded)
{
    gateway::SessionLease lease;
    if (!google::protobuf::util::JsonStringToMessage(encoded, &lease).ok()) {
        throw std::runtime_error("invalid session lease");
    }
    return lease;
}
} // namespace
driver::driver(options config)
    : _options(std::move(config)), _pending_budget(_options.max_pending, _options.max_pending_bytes),
      writes(_options.max_writes, _options.max_write_bytes), reliable(_options.max_pending, _options.max_pending_bytes)
{
}
driver::~driver() = default;
seastar::future<>
driver::start(std::function<gateway::ClientForwardStatus(const gateway::ClientForwardEnvelope&)> forward)
{
    _forward = std::move(forward);
    if (mysql_thread_init() != 0) {
        throw std::runtime_error("MySQL shard initialization failed");
    }
    _mysql_initialized = true;
    if (!_options.mysql.ca_file.empty()) {
        auto pem = co_await seastar::util::read_entire_file_contiguous(_options.mysql.ca_file);
        _options.mysql.ca_pem.assign(pem.data(), pem.size());
    }
    namespace ee = grpc_event_engine::experimental;
    _state_engine = io::make_grpc_engine();
    grpc::ChannelArguments arguments;
    arguments.SetInt(GRPC_ARG_ENABLE_HTTP_PROXY, 0);
    arguments.SetInt(GRPC_ARG_SERVICE_CONFIG_DISABLE_RESOLUTION, 1);
    arguments.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    arguments.SetPointerWithVtable(GRPC_ARG_EVENT_ENGINE, &_state_engine, ee::grpc_event_engine_arg_vtable());
    _state_channel =
        grpc::CreateCustomChannel("dns:///" + _options.state_rpc.host + ":" + std::to_string(_options.state_rpc.port),
                                  grpc::InsecureChannelCredentials(), arguments);
    _state = state::StateService::NewStub(_state_channel);
    _grpc_control.emplace(grpc_control());
    LOG_INFO(logger, "[gateway/io] ready shard={} tid={}", seastar::this_shard_id(), syscall(SYS_gettid));
}
seastar::future<state::MessageTopology> driver::fetch_grpc_topology()
{
    state::TopologyRequest request;
    state::MessageTopology response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    _state_call = &context;
    auto remove = seastar::defer([this]() noexcept { _state_call = nullptr; });
    seastar::promise<grpc::Status> complete;
    auto result = complete.get_future();
    _state->async()->ListMessageNodes(&context, &request, &response,
                                      [&complete](grpc::Status status) { complete.set_value(std::move(status)); });
    const auto status = co_await std::move(result);
    if (!status.ok()) {
        throw std::runtime_error("State topology: " + status.error_message());
    }
    co_return response;
}
seastar::future<> driver::grpc_control()
{
    uint64_t heartbeat{};
    while (!_stopping) {
        try {
            auto topology = co_await fetch_grpc_topology();
            if (_stopping) {
                break;
            }
            std::set<message_node_id> retained;
            for (const auto& node : topology.nodes()) {
                if (node.status() != "active" || node.node_id().empty() || node.host().empty() || node.port() <= 0 ||
                    node.port() > 65535 || node.shard_count() == 0 ||
                    static_cast<uint64_t>(node.port()) + node.shard_count() - 1 > 65535) {
                    continue;
                }
                retained.insert(node.node_id());
                const auto port = static_cast<uint16_t>(
                    node.port() + message_route::owner_shard(_options.gateway_id, node.shard_count()));
                auto found = _message_links.find(node.node_id());
                if (found != _message_links.end() &&
                    (found->second->finished() || found->second->address().host != node.host() ||
                     found->second->address().port != port)) {
                    co_await found->second->close();
                    _message_links.erase(found);
                    found = _message_links.end();
                }
                if (found == _message_links.end()) {
                    auto link =
                        std::make_unique<message_link>(*this, _options, node.node_id(), endpoint{node.host(), port});
                    auto* active = link.get();
                    _message_links.emplace(node.node_id(), std::move(link));
                    active->start();
                }
            }
            for (auto current = _message_links.begin(); current != _message_links.end();) {
                if (!retained.contains(current->first)) {
                    co_await current->second->close();
                    current = _message_links.erase(current);
                } else {
                    ++current;
                }
            }
        } catch (const std::exception& error) {
            if (!_stopping) {
                LOG_WARN(logger, "[gateway/topology] {}", error.what());
            }
        }
        if (_stopping) {
            break;
        }
        for (auto& [id, link] : _message_links) {
            if (!link->healthy()) {
                continue;
            }
            gateway::GatewayToMessageFrame frame;
            frame.mutable_heartbeat()->set_sequence(++heartbeat);
            frame.mutable_heartbeat()->set_sent_at_unix_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                               std::chrono::system_clock::now().time_since_epoch())
                                                               .count());
            try {
                link->enqueue(std::move(frame));
            } catch (const io::overloaded&) {
            }
        }
        try {
            co_await seastar::sleep_abortable(std::chrono::seconds(5), _abort);
        } catch (const seastar::sleep_aborted&) {
            break;
        }
    }
}
seastar::future<> driver::wait_ready()
{
    co_await seastar::with_timeout(
        std::chrono::steady_clock::now() + std::chrono::seconds(30), _grpc_changed.wait([this] {
            return _stopping ||
                   std::ranges::any_of(_message_links, [](const auto& entry) { return entry.second->healthy(); });
        }));
    if (_stopping) {
        throw io::stopped();
    }
}
message_link& driver::select_grpc_link(int64_t conversation_id)
{
    message_link* selected{};
    size_t highest{};
    for (auto& [id, link] : _message_links) {
        if (!link->healthy()) {
            continue;
        }
        if (conversation_id > 0) {
            const auto score = std::hash<std::string>{}(std::to_string(conversation_id) + ":" + id);
            if (!selected || score > highest) {
                selected = link.get();
                highest = score;
            }
        } else if (!selected || link->inflight() < selected->inflight()) {
            selected = link.get();
        }
    }
    if (!selected) {
        throw std::runtime_error("no healthy Message stream");
    }
    return *selected;
}
void driver::on_grpc_frame(message_link& link, gateway::MessageToGatewayFrame frame)
{
    if (frame.has_register_result()) {
        _grpc_changed.broadcast();
        return;
    }
    if (frame.has_command_result()) {
        auto found = _pending.find(frame.command_result().request_id());
        if (found == _pending.end() || found->second.link != &link) {
            return;
        }
        auto node = _pending.extract(found);
        link.remove_inflight();
        auto response = std::move(*frame.mutable_command_result());
        response.set_request_id(node.mapped().client_request_id);
        node.mapped().completion.set_value(std::move(response));
    } else if (frame.has_client_forward()) {
        const auto status = _forward(frame.client_forward());
        gateway::GatewayToMessageFrame ack;
        auto* result = ack.mutable_client_forward_ack();
        result->set_forward_id(frame.client_forward().forward_id());
        result->set_status(status);
        result->set_gateway_id(_options.gateway_id);
        result->set_instance_id(_options.instance_id);
        result->set_recipient_device_id(frame.client_forward().recipient_device_id());
        link.enqueue(std::move(ack));
    }
}
void driver::on_grpc_done(message_link& link)
{
    // 断流只完成属于此实例的请求；已写出的命令结果未知，不统一自动重放业务操作。
    // 客户端得到可重试依赖错误后，仍须遵循具体业务的原有幂等键约束。
    for (auto entry = _pending.begin(); entry != _pending.end();) {
        if (entry->second.link == &link) {
            entry->second.completion.set_exception(std::runtime_error("Message stream closed before result"));
            entry = _pending.erase(entry);
            link.remove_inflight();
        } else {
            ++entry;
        }
    }
    _grpc_changed.broadcast();
}
seastar::future<command_ticket> driver::execute(send_command operation)
{
    if (_stopping) {
        throw io::stopped();
    }
    auto& link = select_grpc_link(operation.command.conversation_id());
    auto units = _pending_budget.acquire(operation.command.ByteSizeLong());
    const auto client_id = operation.command.request_id();
    const auto correlation = operation.command.connection_id() + ":" + client_id;
    if (_pending.contains(correlation)) {
        throw io::overloaded();
    }
    operation.command.set_request_id(correlation);
    pending_command pending{&link, client_id, std::move(units), {}};
    auto result = pending.completion.get_future();
    gateway::GatewayToMessageFrame frame;
    *frame.mutable_command() = std::move(operation.command);
    // 先申请并排队，成功后登记 promise；StartWrite 的完成事件不会在此处重入协议回调。
    link.enqueue(std::move(frame));
    _pending.emplace(correlation, std::move(pending));
    link.add_inflight();
    co_return command_ticket{std::move(result)};
}
seastar::future<endpoint> driver::discover_redis_master()
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
    const auto address = co_await discover_redis_master();
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
seastar::future<bool> driver::execute(validate_token operation)
{
    std::vector<std::string> command{"GET", "im:chatAuth:" + std::to_string(operation.uid)};
    auto reply = co_await redis_command(std::move(command));
    check_reply(reply.type == REDIS_REPLY_STRING || reply.type == REDIS_REPLY_NIL);
    co_return reply.type == REDIS_REPLY_STRING&& reply.text == operation.token;
}
seastar::future<bool> driver::execute(prepare_profile operation)
{
    if (operation.login.init_profile()) {
        std::vector<mysql_parameter> parameters;
        parameters.emplace_back(operation.login.uid());
        parameters.emplace_back(operation.login.name());
        parameters.emplace_back(static_cast<int64_t>(operation.login.age()));
        parameters.emplace_back(operation.login.sex());
        parameters.emplace_back(operation.login.head_image_url());
        co_await mysql_query("INSERT INTO userInfo(uid,name,age,sex,headImageURL) VALUES(?,?,?,?,?)",
                             std::move(parameters));
    }
    std::vector<mysql_parameter> parameters;
    parameters.emplace_back(operation.login.uid());
    auto reply = co_await mysql_query("SELECT 1 FROM userInfo WHERE uid = ? LIMIT 1", std::move(parameters));
    co_return !reply.row.empty();
}
seastar::future<bool> driver::execute(register_device operation)
{
    std::vector<mysql_parameter> parameters;
    parameters.emplace_back(operation.device_id);
    parameters.emplace_back(operation.uid);
    parameters.emplace_back(std::string{});
    parameters.emplace_back(std::string{});
    co_await mysql_query("INSERT INTO devices(deviceId,uid,platform,deviceName) VALUES(?,?,?,?) ON DUPLICATE KEY "
                         "UPDATE deviceId=VALUES(deviceId)",
                         std::move(parameters));
    parameters.clear();
    parameters.emplace_back(std::move(operation.device_id));
    auto reply = co_await mysql_query("SELECT uid FROM devices WHERE deviceId = ? LIMIT 1", std::move(parameters));
    co_return reply.row.size() == 1 && std::stoll(reply.row[0]) == operation.uid;
}
seastar::future<bound_session> driver::execute(bind_session operation)
{
    const auto& route = operation.route;
    const auto suffix = std::to_string(operation.uid) + ":" + route.device_id();
    std::vector<std::string> command{"EVAL",
                                     R"(
local previous = redis.call('GET', KEYS[2]) or ''
local generation = redis.call('INCR', KEYS[1])
local value = cjson.encode({deviceId=ARGV[1],gatewayId=ARGV[2],instanceId=ARGV[3],connectionId=ARGV[4],generation=generation,shardId=tonumber(ARGV[5])})
redis.call('SETEX', KEYS[2], 60, value)
redis.call('SADD', KEYS[3], ARGV[1])
redis.call('EXPIRE', KEYS[3], 120)
return {tostring(generation),previous}
)",
                                     "3",
                                     "im:sessionGeneration:" + suffix,
                                     lease_key(operation.uid, route.device_id()),
                                     "im:userSessions:" + std::to_string(operation.uid),
                                     route.device_id(),
                                     route.gateway_id(),
                                     route.instance_id(),
                                     route.connection_id(),
                                     std::to_string(route.shard_id())};
    auto reply = co_await redis_command(std::move(command));
    check_reply(reply.type == REDIS_REPLY_ARRAY && reply.elements.size() == 2);
    bound_session result{std::move(operation.route), {}};
    result.current.set_generation(std::stoull(reply.elements[0]));
    if (!reply.elements[1].empty()) {
        result.previous = decode_lease(reply.elements[1]);
    }
    co_return result;
}
seastar::future<bool> driver::execute(refresh_session operation)
{
    const auto& route = operation.route;
    std::vector<std::string> command{"EVAL",
                                     R"(
local value = redis.call('GET', KEYS[1])
if not value then return 0 end
local lease = cjson.decode(value)
if lease.connectionId ~= ARGV[1] or tostring(lease.generation) ~= ARGV[2] or lease.instanceId ~= ARGV[3] or lease.shardId ~= tonumber(ARGV[4]) then return 0 end
redis.call('EXPIRE', KEYS[1], 60)
redis.call('SADD', KEYS[2], ARGV[5])
redis.call('EXPIRE', KEYS[2], 120)
return 1
)",
                                     "2",
                                     lease_key(operation.uid, route.device_id()),
                                     "im:userSessions:" + std::to_string(operation.uid),
                                     route.connection_id(),
                                     std::to_string(route.generation()),
                                     route.instance_id(),
                                     std::to_string(route.shard_id()),
                                     route.device_id()};
    auto reply = co_await redis_command(std::move(command));
    check_reply(reply.type == REDIS_REPLY_INTEGER);
    co_return reply.integer == 1;
}
seastar::future<> driver::execute(clear_session operation)
{
    const auto& route = operation.route;
    std::vector<std::string> command{"EVAL",
                                     R"(
local value = redis.call('GET', KEYS[1])
if not value then redis.call('SREM', KEYS[2], ARGV[4]) return 1 end
local lease = cjson.decode(value)
if lease.connectionId ~= ARGV[1] or tostring(lease.generation) ~= ARGV[2] or lease.instanceId ~= ARGV[3] then return 0 end
redis.call('DEL', KEYS[1])
redis.call('SREM', KEYS[2], ARGV[4])
return 1
)",
                                     "2",
                                     lease_key(operation.uid, route.device_id()),
                                     "im:userSessions:" + std::to_string(operation.uid),
                                     route.connection_id(),
                                     std::to_string(route.generation()),
                                     route.instance_id(),
                                     route.device_id()};
    auto reply = co_await redis_command(std::move(command));
    check_reply(reply.type == REDIS_REPLY_INTEGER);
}
void driver::cancel(const std::string& connection_id)
{
    const auto prefix = connection_id + ":";
    for (auto entry = _pending.begin(); entry != _pending.end();) {
        if (entry->first.starts_with(prefix)) {
            entry->second.link->remove_inflight();
            entry->second.completion.set_exception(io::stopped());
            entry = _pending.erase(entry);
        } else {
            ++entry;
        }
    }
}
void driver::abort()
{
    if (_stopping) {
        return;
    }
    _stopping = true;
    _abort.request_abort();
    _grpc_changed.broadcast();
    if (_state_call) {
        _state_call->TryCancel();
    }
    for (auto& [id, link] : _message_links) {
        link->stop();
    }
    _dns_closed.emplace(_dns.close());
    for (auto* connection : _redis) {
        connection->abort();
    }
    for (auto* connection : _mysql) {
        connection->abort();
    }
}
seastar::future<> driver::stop()
{
    abort();
    if (_grpc_control) {
        co_await std::move(*_grpc_control);
        _grpc_control.reset();
    }
    for (auto& [id, link] : _message_links) {
        co_await link->close();
    }
    _message_links.clear();
    assert(_pending.empty() && _redis.empty() && _mysql.empty());
    _state.reset();
    _state_channel.reset();
    if (_state_engine) {
        co_await io::close_grpc_engine(_state_engine);
        _state_engine.reset();
    }
    if (_dns_closed) {
        co_await std::move(*_dns_closed);
        _dns_closed.reset();
    }
    co_await io::drain_local_grpc();
    if (_mysql_initialized) {
        mysql_thread_end();
        _mysql_initialized = false;
    }
}
} // namespace imm::gateway_io
