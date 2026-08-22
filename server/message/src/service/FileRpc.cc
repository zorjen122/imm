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
        LOG_INFO(logger, "[message/file-rpc-start] 文件 RPC 连接池已创建, host: {}, port: {}, connections: {}", host,
                 rpcPort, pool->getPoolSize());
    } catch (std::exception& e) {
        LOG_ERROR(logger, "[message/file-rpc-start] 文件 RPC 连接池创建失败, error: {}", e.what());
    }
}
FileRpc::~FileRpc() { LOG_INFO(logger, "[message/file-rpc-stop] 文件 RPC 连接池已销毁"); }
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
        LOG_WARN(logger, "[message/file-rpc-upload] 没有可用的文件 RPC 连接");
        return grpc::Status(grpc::StatusCode::INTERNAL, "No available connection");
    }
    Defer defer([&]() { pool->returnConnection(std::move(rpc)); });

    auto status = rpc->Upload(&context, req, &resp);
    if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        Metrics::Increment(Metric::RpcDeadlineExceeded);
        auto* requestContext = RequestContextScope::Current();
        LOG_WARN(logger, "[message/file-rpc-upload] 文件上传请求超时, request_id: {}",
                 requestContext == nullptr ? "" : requestContext->requestId);
    } else if (!status.ok()) {
        LOG_WARN(logger, "[message/file-rpc-upload] 文件上传请求失败, status: {}, error: {}",
                 static_cast<int>(status.error_code()), status.error_message());
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
    if (rpc == nullptr) {
        LOG_WARN(logger, "[message/file-rpc-download] 没有可用的文件 RPC 连接");
        return grpc::Status(grpc::StatusCode::INTERNAL, "No available connection");
    }
    Defer defer([&]() { pool->returnConnection(std::move(rpc)); });
    const auto status = rpc->Download(&context, req, &resp);
    if (!status.ok())
        LOG_WARN(logger, "[message/file-rpc-download] 文件下载请求失败, status: {}, error: {}",
                 static_cast<int>(status.error_code()), status.error_message());
    return status;
}

auto FileRpc::getPoolSize() const { return pool->getPoolSize(); }
}; // namespace wimi::rpc
