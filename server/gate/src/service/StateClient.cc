#include "StateClient.h"
#include "Configer.h"
#include "Const.h"
#include "Logger.h"
#include "spdlog/logger.h"
#include "state.grpc.pb.h"
#include <grpcpp/client_context.h>

namespace wimi::rpc
{
namespace
{

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
    auto status = caller->PickConnectionGateway(&context, req, &rsp);
    if (!status.ok())
        return {};
    return ServerNode(rsp.ip(), rsp.port(), rsp.node_id());
}

}; // namespace wimi::rpc
