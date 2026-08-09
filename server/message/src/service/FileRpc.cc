#include "FileRpc.h"
#include "Configer.h"
#include "Logger.h"
#include "Metrics.h"
#include "RpcPool.h"
#include "RequestContext.h"
#include <exception>
#include <grpcpp/client_context.h>
#include <string>
namespace wimi::rpc
{
FileRpc::FileRpc()
{
    try {
        auto conf = Configer::getNode("server");
        std::string host = conf["file"]["host"].as<std::string>();
        unsigned short rpcPort = conf["file"]["rpcPort"].as<int>();
        int rpcCount = conf["file"]["rpcCount"].as<int>();
        pool.reset(new RpcPool<FileService>(rpcCount, host, std::to_string(rpcPort)));
        LOG_INFO(netLogger, "FileRpc::FileRpc() | host: {}, rpcPort: {}, rpcCount: {}", host, rpcPort,
                 pool->getPoolSize());
    } catch (std::exception& e) {
        LOG_ERROR(netLogger, "FileRpc::FileRpc() is wrong | what: {}", e.what());
    }
}
FileRpc::~FileRpc() { LOG_INFO(netLogger, "FileRpc::~FileRpc()"); }
grpc::Status FileRpc::forwardUpload(const UploadRequest& req, UploadResponse& resp)
{
    ClientContext context;
    // 文件 RPC 与文本 RPC 共享请求预算，不能在上游超时后继续长期占用线程。
    if (auto* requestContext = RequestContextScope::Current();
        requestContext != nullptr && requestContext->deadline != RequestContext::Deadline::max()) {
        context.set_deadline(requestContext->SystemDeadline());
    }
    auto rpc = pool->getConnection();
    if (rpc == nullptr) {
        LOG_INFO(netLogger, "FileRpc::forwardUpload() | No available connection");
        return grpc::Status(grpc::StatusCode::INTERNAL, "No available connection");
    }
    Defer defer([&]() { pool->returnConnection(std::move(rpc)); });

    auto status = rpc->Upload(&context, req, &resp);
    if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        Metrics::Increment(Metric::RpcDeadlineExceeded);
        auto* requestContext = RequestContextScope::Current();
        LOG_WARN(netLogger, "文件上传RPC超过请求截止时间, requestId: {}",
                 requestContext == nullptr ? "" : requestContext->requestId);
    }
    return status;
}

grpc::Status FileRpc::forwardDownload(const DownloadRequest& req, DownloadResponse& resp)
{
    ClientContext context;
    if (auto* requestContext = RequestContextScope::Current();
        requestContext != nullptr && requestContext->deadline != RequestContext::Deadline::max())
        context.set_deadline(requestContext->SystemDeadline());
    auto rpc = pool->getConnection();
    if (rpc == nullptr)
        return grpc::Status(grpc::StatusCode::INTERNAL, "No available connection");
    Defer defer([&]() { pool->returnConnection(std::move(rpc)); });
    return rpc->Download(&context, req, &resp);
}

auto FileRpc::getPoolSize() const { return pool->getPoolSize(); }
}; // namespace wimi::rpc
