#include "io/driver.hh"
#include <fmt/format.h>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/create_channel.h>
#include <grpc/impl/channel_arg_names.h>
#include <seastar/core/sleep.hh>
#include <seastar/util/defer.hh>
#include <array>
#include <cassert>
#include <chrono>

namespace imm::message_io
{
namespace
{
void require_reply(bool condition)
{
    if (!condition) {
        throw std::runtime_error("invalid Redis response");
    }
}
std::string session_key(int64_t uid, const std::string& device_id)
{
    return "im:session:" + std::to_string(uid) + ":" + device_id;
}
} // namespace
driver::driver(options config) : _options(std::move(config)) {}
driver::~driver() { assert(!_mysql && !_redis && !_file && !_file_engine && !_mysql_initialized); }
void driver::check_running() const
{
    if (_stopping) {
        throw io::stopped();
    }
}
seastar::future<> driver::start()
{
    if (mysql_thread_init() != 0) {
        throw std::runtime_error("MySQL shard initialization failed");
    }
    _mysql_initialized = true;
    namespace ee = grpc_event_engine::experimental;
    _file_engine = io::make_grpc_engine();
    grpc::ChannelArguments arguments;
    arguments.SetInt(GRPC_ARG_ENABLE_HTTP_PROXY, 0);
    arguments.SetInt(GRPC_ARG_SERVICE_CONFIG_DISABLE_RESOLUTION, 1);
    arguments.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    arguments.SetPointerWithVtable(GRPC_ARG_EVENT_ENGINE, &_file_engine, ee::grpc_event_engine_arg_vtable());
    _file_channel =
        grpc::CreateCustomChannel("dns:///" + _options.file_rpc.host + ":" + std::to_string(_options.file_rpc.port),
                                  grpc::InsecureChannelCredentials(), arguments);
    _file = file::FileService::NewStub(_file_channel);
    co_return;
}
seastar::future<> driver::connect_mysql()
{
    check_running();
    _mysql = std::make_unique<io::mysql_connection>();
    try {
        const auto ip = co_await _dns.resolve_name(_options.mysql.address.host);
        check_running();
        co_await _mysql->connect(fmt::format("{}", ip), _options.mysql);
    } catch (...) {
        _mysql->abort();
        throw;
    }
}
seastar::future<io::endpoint> driver::discover_redis_master()
{
    std::exception_ptr failure;
    for (const io::endpoint& address : _options.sentinels) {
        check_running();
        io::redis_connection connection;
        _sentinel = &connection;
        auto remove = seastar::defer([this]() noexcept { _sentinel = nullptr; });
        std::optional<io::endpoint> result;
        try {
            const auto ip = co_await _dns.resolve_name(address.host);
            check_running();
            co_await connection.connect(fmt::format("{}", ip), address.port);
            if (!_options.sentinel_password.empty()) {
                std::vector<std::string> auth{"AUTH", _options.sentinel_password};
                auto reply = co_await connection.command(std::move(auth));
                require_reply(reply.type == REDIS_REPLY_STATUS && reply.text == "OK");
            }
            std::vector<std::string> command{"SENTINEL", "get-master-addr-by-name", _options.master_name};
            auto reply = co_await connection.command(std::move(command));
            require_reply(reply.type == REDIS_REPLY_ARRAY && reply.elements.size() == 2);
            const auto port = std::stoul(reply.elements[1]);
            require_reply(port && port <= 65535);
            result = io::endpoint{std::move(reply.elements[0]), static_cast<uint16_t>(port)};
        } catch (...) {
            failure = std::current_exception();
        }
        co_await connection.close();
        if (result) {
            co_return std::move(*result);
        }
    }
    std::rethrow_exception(failure);
}
seastar::future<io::redis_reply> driver::redis_command(std::vector<std::string> arguments)
{
    check_running();
    std::optional<io::redis_reply> result;
    std::exception_ptr failure;
    try {
        // 复用主节点连接，但每次操作仍通过 Sentinel 确认主地址。发生切换时先丢弃
        // 旧连接，避免旧主变为副本后继续接受 GET 而返回过期 lease；不重放已发出的命令。
        auto address = co_await discover_redis_master();
        if (_redis && (_redis_address->host != address.host || _redis_address->port != address.port)) {
            co_await _redis->close();
            _redis.reset();
        }
        if (!_redis) {
            auto ip = co_await _dns.resolve_name(address.host);
            check_running();
            _redis_address = address;
            _redis = std::make_unique<io::redis_connection>();
            co_await _redis->connect(fmt::format("{}", ip), address.port);
            if (!_options.redis_password.empty()) {
                std::vector<std::string> auth{"AUTH", _options.redis_password};
                auto reply = co_await _redis->command(std::move(auth));
                require_reply(reply.type == REDIS_REPLY_STATUS && reply.text == "OK");
            }
        }
        result = co_await _redis->command(std::move(arguments));
        require_reply(result->type != REDIS_REPLY_ERROR);
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        if (_redis) {
            co_await _redis->close();
            _redis.reset();
        }
        std::rethrow_exception(failure);
    }
    co_return std::move(*result);
}
seastar::future<gateway::SessionLease> driver::session(int64_t uid, std::string device_id)
{
    std::vector<std::string> command{"GET", session_key(uid, device_id)};
    auto reply = co_await redis_command(std::move(command));
    gateway::SessionLease lease;
    if (reply.type == REDIS_REPLY_NIL) {
        co_return lease;
    }
    require_reply(reply.type == REDIS_REPLY_STRING);
    if (!google::protobuf::util::JsonStringToMessage(reply.text, &lease).ok()) {
        throw std::runtime_error("invalid session lease");
    }
    co_return lease;
}
seastar::future<gateway::SessionLease> driver::execute(get_session operation)
{
    auto exclusive = co_await seastar::get_units(_redis_slot, 1);
    check_running();
    co_return co_await session(operation.uid, std::move(operation.device_id));
}
seastar::future<std::vector<gateway::SessionLease>> driver::execute(get_sessions operation)
{
    auto exclusive = co_await seastar::get_units(_redis_slot, 1);
    check_running();
    const auto key = "im:userSessions:" + std::to_string(operation.uid);
    std::vector<std::string> command{"SMEMBERS", key};
    auto reply = co_await redis_command(std::move(command));
    require_reply(reply.type == REDIS_REPLY_ARRAY);
    std::vector<gateway::SessionLease> result;
    for (const std::string& device_id : reply.elements) {
        auto lease = co_await session(operation.uid, device_id);
        if (lease.connection_id().empty()) {
            std::vector<std::string> remove{"SREM", key, device_id};
            co_await redis_command(std::move(remove));
        } else {
            result.push_back(std::move(lease));
        }
    }
    co_return result;
}
seastar::future<int64_t> driver::execute(generate_id operation)
{
    auto exclusive = co_await seastar::get_units(_redis_slot, 1);
    check_running();
    constexpr std::array prefixes{"im:msgId", "im:conversationId", "im:friendshipId", "im:groupId"};
    // 保留现有 ID 的时间戳、机器位与序列位布局。序列耗尽时异步等待下一毫秒，
    // 不在 reactor 内 sleep_for，也不让同一 shard 的其他 I/O 消费者忙轮询。
    while (true) {
        const auto timestamp =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count() -
            1672531200000;
        const std::string key =
            std::string(prefixes.at(static_cast<size_t>(operation.kind))) + ":" + std::to_string(timestamp);
        std::vector<std::string> command{"INCR", key};
        auto reply = co_await redis_command(std::move(command));
        require_reply(reply.type == REDIS_REPLY_INTEGER && reply.integer > 0);
        if (reply.integer <= 4095) {
            co_return (timestamp << 22) | (static_cast<int64_t>(_options.machine_id) << 12) | reply.integer;
        }
        co_await seastar::sleep_abortable(std::chrono::milliseconds(1), _abort);
    }
}
seastar::future<file::UploadResponse> driver::execute(upload_file operation)
{
    check_running();
    grpc::ClientContext context;
    _file_calls.insert(&context);
    auto remove = seastar::defer([this, &context]() noexcept { _file_calls.erase(&context); });
    file::UploadResponse response;
    seastar::promise<grpc::Status> completion;
    auto result = completion.get_future();
    _file->async()->Upload(&context, &operation.request, &response,
                           [&completion](grpc::Status status) { completion.set_value(std::move(status)); });
    const auto status = co_await std::move(result);
    if (!status.ok()) {
        throw std::runtime_error("File upload RPC: " + status.error_message());
    }
    co_return response;
}
seastar::future<file::DownloadResponse> driver::execute(download_file operation)
{
    check_running();
    grpc::ClientContext context;
    _file_calls.insert(&context);
    auto remove = seastar::defer([this, &context]() noexcept { _file_calls.erase(&context); });
    file::DownloadResponse response;
    seastar::promise<grpc::Status> completion;
    auto result = completion.get_future();
    _file->async()->Download(&context, &operation.request, &response,
                             [&completion](grpc::Status status) { completion.set_value(std::move(status)); });
    const auto status = co_await std::move(result);
    if (!status.ok()) {
        throw std::runtime_error("File download RPC: " + status.error_message());
    }
    co_return response;
}
void driver::abort()
{
    if (_stopping) {
        return;
    }
    _stopping = true;
    _abort.request_abort();
    _dns_closed.emplace(_dns.close());
    _mysql_slot.broken();
    _redis_slot.broken();
    if (_mysql) {
        _mysql->abort();
    }
    if (_redis) {
        _redis->abort();
    }
    if (_sentinel) {
        _sentinel->abort();
    }
    for (grpc::ClientContext* context : _file_calls) {
        context->TryCancel();
    }
}
seastar::future<> driver::close()
{
    abort();
    // 所有者已经等待队列消费者退出，此处不再存在借用连接或 RPC 状态的协程。
    assert(_file_calls.empty() && !_sentinel);
    if (_redis) {
        co_await _redis->close();
        _redis.reset();
    }
    if (_mysql) {
        co_await _mysql->close();
        _mysql.reset();
    }
    _file.reset();
    _file_channel.reset();
    if (_file_engine) {
        co_await io::close_grpc_engine(_file_engine);
        _file_engine.reset();
    }
    co_await std::move(*_dns_closed);
    _dns_closed.reset();
    if (_mysql_initialized) {
        mysql_thread_end();
        _mysql_initialized = false;
    }
    // 本 shard 的服务端 listener 可能仍存活，由最外层在服务端关闭后统一 drain。
}
} // namespace imm::message_io
