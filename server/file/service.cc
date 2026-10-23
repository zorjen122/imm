#include "service.hh"
#include "Logger.h"
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <fmt/format.h>
#include <cassert>
#include <exception>
#include <utility>
#include <type_traits>

namespace imm::file_service
{
class unary_call final : public grpc::ServerUnaryReactor
{
    seastar::gate::holder _lifetime;
    seastar::semaphore_units<> _requests;
    seastar::semaphore_units<> _bytes;
    const unsigned _owner{seastar::this_shard_id()};

  public:
    unary_call(seastar::gate::holder lifetime, seastar::semaphore_units<> requests, seastar::semaphore_units<> bytes)
        : _lifetime(std::move(lifetime)), _requests(std::move(requests)), _bytes(std::move(bytes))
    {
    }
    void OnDone() override
    {
        assert(seastar::engine_is_ready() && seastar::this_shard_id() == _owner);
        delete this;
    }
};
service::service(options config, scheduling_groups groups)
    : _options(std::move(config)), _groups(groups), _storage(_options.storage_root), _requests(_options.max_requests),
      _request_bytes(_options.max_bytes), _operations(_options.max_requests), _operation_bytes(_options.max_bytes)
{
}
seastar::future<> service::start()
{
    co_await _storage.start();
    const auto ip = co_await _dns.resolve_name(_options.listener.host);
    _server.start({fmt::format("{}", ip), _options.listener.port}, grpc::InsecureServerCredentials(), *this, true);
    LOG_INFO(logger, "[file/io] ready shard={} port={}", seastar::this_shard_id(), _server.port());
}
seastar::future<response> service::dispatch(request operation)
{
    if (auto* upload = std::get_if<::file::UploadRequest>(&operation)) {
        co_return response(co_await _storage.upload(std::move(*upload)));
    }
    co_return response(co_await _storage.download(std::move(std::get<::file::DownloadRequest>(operation))));
}
seastar::future<result> service::store(request operation, std::size_t bytes)
{
    if (_stopping) {
        co_return result{grpc::Status(grpc::StatusCode::UNAVAILABLE, "File is stopping"), {}};
    }
    auto capacity = seastar::try_get_units(_operations, 1);
    auto memory = seastar::try_get_units(_operation_bytes, bytes);
    if (!capacity || !memory) {
        co_return result{grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "File storage capacity exceeded"), {}};
    }
    auto lifetime = _storage_tasks.hold();
    const auto key =
        std::visit([](const auto& input) { return object_key(input.user_id(), input.file().file_id()); }, operation);
    auto [position, inserted] = _objects.try_emplace(key);
    if (inserted) {
        position->second = seastar::make_lw_shared<object_state>();
    }
    auto state = position->second;
    // 只有 key owner 持有文件和互斥状态；等待 key 锁的请求同样占用有界存储额度。
    auto exclusive = co_await seastar::get_units(state->exclusive, 1);
    response output;
    std::exception_ptr failure;
    try {
        output = co_await seastar::with_scheduling_group(
            _groups.io, [this, operation = std::move(operation)]() mutable { return dispatch(std::move(operation)); });
    } catch (...) {
        failure = std::current_exception();
    }
    exclusive.return_all();
    // map 与本请求是最后两个引用时，不再有等待者；不永久积累历史 file-id。
    if (state.use_count() == 2) {
        _objects.erase(key);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    co_return result{grpc::Status::OK, std::move(output)};
}
seastar::future<> service::execute(unary_call* call, request operation, std::size_t bytes,
                                   std::function<void(response)> deliver, seastar::gate::holder lifetime)
{
    grpc::Status status;
    try {
        const auto checked = co_await seastar::with_scheduling_group(_groups.protocol, [&operation] {
            return seastar::make_ready_future<bool>(
                std::visit([](const auto& input) { return valid(input); }, operation));
        });
        if (!checked) {
            // 字段错误沿用业务 FAILED；gRPC 状态继续为 OK。
            deliver(std::visit(
                [](const auto& input) -> response {
                    using Request = std::decay_t<decltype(input)>;
                    if constexpr (std::is_same_v<Request, ::file::UploadRequest>) {
                        ::file::UploadResponse output;
                        output.set_user_id(input.user_id());
                        output.set_status(::file::FAILED);
                        output.set_message("invalid file upload request");
                        return output;
                    } else {
                        ::file::DownloadResponse output;
                        output.set_status(::file::FAILED);
                        output.set_message("invalid file download request");
                        return output;
                    }
                },
                operation));
        } else {
            const auto key = std::visit(
                [](const auto& input) { return object_key(input.user_id(), input.file().file_id()); }, operation);
            const auto owner = owner_shard(key, seastar::this_smp_shard_count());
            auto output = co_await seastar::with_scheduling_group(
                _groups.transaction, [this, owner, operation = std::move(operation), bytes]() mutable {
                    return container().invoke_on(owner,
                                                 [operation = std::move(operation), bytes](service& local) mutable {
                                                     return local.store(std::move(operation), bytes);
                                                 });
                });
            status = std::move(output.status);
            if (status.ok()) {
                deliver(std::move(output.payload));
            }
        }
    } catch (const std::exception& error) {
        status = grpc::Status(grpc::StatusCode::INTERNAL, error.what());
    }
    // response 和 Finish 仅在接入 shard 使用；跨 shard 传递的是独立 protobuf 值。
    // gRPC 负责至 OnDone 的消息寿命，任务另持 gate，避免 Finish 后服务提前析构。
    co_await seastar::with_scheduling_group(_groups.io, [call, status = std::move(status)] { call->Finish(status); });
}
template <typename Request, typename Response>
grpc::ServerUnaryReactor* service::submit(grpc::CallbackServerContext& context, const Request& input, Response& output)
{
    auto reject = [&context](grpc::StatusCode code, const char* message) {
        auto* call = context.DefaultReactor();
        call->Finish(grpc::Status(code, message));
        return call;
    };
    if (_stopping) {
        return reject(grpc::StatusCode::UNAVAILABLE, "File is stopping");
    }
    const auto bytes = input.ByteSizeLong() + max_chunk_size + 65536;
    auto capacity = seastar::try_get_units(_requests, 1);
    auto memory = seastar::try_get_units(_request_bytes, bytes);
    if (!capacity || !memory) {
        return reject(grpc::StatusCode::RESOURCE_EXHAUSTED, "File request capacity exceeded");
    }
    auto* call = new unary_call(_calls.hold(), std::move(*capacity), std::move(*memory));
    (void)execute(
        call, request(input), bytes, [&output](response value) { output = std::move(std::get<Response>(value)); },
        _tasks.hold());
    return call;
}
grpc::ServerUnaryReactor* service::Upload(grpc::CallbackServerContext* context, const ::file::UploadRequest* input,
                                          ::file::UploadResponse* output)
{
    output->set_user_id(input->user_id());
    return submit(*context, *input, *output);
}
grpc::ServerUnaryReactor* service::Download(grpc::CallbackServerContext* context, const ::file::DownloadRequest* input,
                                            ::file::DownloadResponse* output)
{
    return submit(*context, *input, *output);
}
seastar::future<> service::stop()
{
    if (std::exchange(_stopped, true)) {
        co_return;
    }
    _stopping = true;
    co_await _server.close([this]() -> seastar::future<> {
        co_await _tasks.close();
        co_await _calls.close();
    });
    co_await _storage_tasks.close();
    co_await _dns.close();
    co_await io::drain_local_grpc();
}
} // namespace imm::file_service
