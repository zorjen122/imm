#include "reactor/grpc_server.hh"
#include <grpc/grpc.h>
#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/impl/server_builder_option.h>
#include <grpcpp/server_builder.h>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <cassert>

namespace imm::io
{
namespace
{
namespace ee = grpc_event_engine::experimental;
struct completion : grpc_completion_queue_functor {
    seastar::promise<> done;
    completion()
    {
        functor_run = [](grpc_completion_queue_functor* source, int success) {
            assert(seastar::engine_is_ready() && success);
            static_cast<completion*>(source)->done.set_value();
        };
        inlineable = 0;
    }
};
class engine_option final : public grpc::ServerBuilderOption
{
    std::shared_ptr<ee::EventEngine> _engine;

  public:
    explicit engine_option(std::shared_ptr<ee::EventEngine> engine) : _engine(std::move(engine)) {}
    void UpdateArguments(grpc::ChannelArguments* arguments) override
    {
        arguments->SetPointerWithVtable(GRPC_ARG_EVENT_ENGINE, &_engine, ee::grpc_event_engine_arg_vtable());
    }
    void UpdatePlugins(std::vector<std::unique_ptr<grpc::ServerBuilderPlugin>>*) override {}
};
} // namespace
grpc_server::~grpc_server() { assert(!_server && !_engine); }
void grpc_server::start(endpoint address, std::shared_ptr<grpc::ServerCredentials> credentials, grpc::Service& service,
                        bool reuse_port)
{
    assert(!_server && !_engine);
    _engine = make_grpc_engine();
    grpc::ServerBuilder builder;
    builder.SetOption(std::make_unique<engine_option>(_engine));
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, reuse_port ? 1 : 0);
    const auto host = address.host.find(':') == std::string::npos ? address.host : "[" + address.host + "]";
    int port{};
    builder.AddListeningPort(host + ":" + std::to_string(address.port), credentials, &port);
    builder.RegisterService(&service);
    _server = builder.BuildAndStart();
    if (!_server) {
        throw std::runtime_error("gRPC reactor server startup failed");
    }
    _port = port;
}
seastar::future<> grpc_server::close(std::function<seastar::future<>()> wait_calls)
{
    if (_server) {
        completion notified, closed;
        auto notify_done = notified.done.get_future();
        auto close_done = closed.done.get_future();
        auto* queue = grpc_completion_queue_create_for_callback(&closed, nullptr);
        grpc_server_shutdown_and_notify(_server->c_server(), queue, &notified);
        grpc_server_cancel_all_calls(_server->c_server());
        co_await std::move(notify_done);
        grpc_completion_queue_shutdown(queue);
        co_await std::move(close_done);
        grpc_completion_queue_destroy(queue);
        co_await wait_calls();
        // core 与应用回调均已结束；C++ 层这里只收束已完成的状态和 callback CQ。
        _server->Shutdown();
        _server.reset();
    }
    if (_engine) {
        co_await close_grpc_engine(_engine);
        _engine.reset();
    }
}
} // namespace imm::io
