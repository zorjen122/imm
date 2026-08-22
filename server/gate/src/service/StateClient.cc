#include "StateClient.h"
#include "Configer.h"
#include "Const.h"
#include "Logger.h"
#include "Metrics.h"
#include "RequestContext.h"
#include "spdlog/logger.h"
#include "state.grpc.pb.h"
#include <grpcpp/client_context.h>
#include <algorithm>
#include <chrono>

namespace wimi::rpc
{
namespace
{

void ApplyDeadline(grpc::ClientContext& context)
{
    // Gate 尚未建立 HTTP RequestContext 时使用 1 秒保底，禁止控制面 RPC
    // 无限等待。
    auto deadline = RequestContextScope::CurrentDeadlineOr(std::chrono::milliseconds(1000));
    auto remaining =
        std::max(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - RequestContext::Clock::now()),
                 std::chrono::milliseconds(0));
    context.set_deadline(std::chrono::system_clock::now() + remaining);
}

void RecordStatus(const grpc::Status& status)
{
    if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        Metrics::Increment(Metric::RpcDeadlineExceeded);
        LOG_WARN(logger, "Gate调用State RPC超时");
    }
}

} // namespace

StateClient::StateClient()
{
    auto conf = Configer::getNode("server");
    auto host = conf["stateRPC"]["host"].as<std::string>();
    auto port = conf["stateRPC"]["port"].as<std::string>();
    auto rpcCount = conf["stateRPC"]["rpcCount"].as<int>();
    rpcPool.reset(new RpcPool<StateService>(rpcCount, host, port));

    if (rpcPool->empty()) {
        logger->warn("StateClient init failed, host: {}, port: {}, rpcCount: {}");
        return;
    }
    logger->info("StateClient init success, host: {}, port: {}, rpcCount: {}", host, port, rpcPool->getPoolSize());
}

ServerNode StateClient::PickConnectionGateway(int uid)
{
    state::ConnectUser req;
    state::ConnectUserRsp rsp;
    req.set_id(uid);
    auto caller = rpcPool->getConnection();
    if (!caller)
        return {};
    Defer defer([&caller, this]() { rpcPool->returnConnection(std::move(caller)); });

    grpc::ClientContext context;
    ApplyDeadline(context);
    auto status = caller->PickConnectionGateway(&context, req, &rsp);
    RecordStatus(status);
    if (!status.ok())
        return {};
    return ServerNode(rsp.ip(), rsp.port(), rsp.node_id());
}

}; // namespace wimi::rpc
