#pragma once
#include "reactor/types.hh"
#include "reactor/grpc_engine.hh"
#include <grpcpp/server.h>
#include <grpcpp/security/server_credentials.h>
#include <functional>

namespace imm::io
{
// 在所属 reactor 构造/启动/关闭。凭据须在准入前加载，address.host 必须为数字 IP。
// close 的回调在 core 停止接收和取消 RPC 后执行，须等待全部应用 OnDone 结束；
// 此后才释放 C++ Server，避免在 reactor 上等待尚未被调度的完成回调。
class grpc_server
{
    std::shared_ptr<grpc_event_engine::experimental::EventEngine> _engine;
    std::unique_ptr<grpc::Server> _server;
    uint16_t _port{};

  public:
    ~grpc_server();
    void start(endpoint address, std::shared_ptr<grpc::ServerCredentials> credentials, grpc::Service& service,
               bool reuse_port = false);
    uint16_t port() const { return _port; }
    seastar::future<> close(std::function<seastar::future<>()> wait_calls);
};
} // namespace imm::io
