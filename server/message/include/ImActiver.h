#pragma once
// MessageServiceManager.h
#include "Configer.h"
#include "Const.h"
#include "Logger.h"
#include "GatewayStreamService.h"
#include "GrpcSecurity.h"
#include "Mysql.h"
#include "Redis.h"
#include "Service.h"

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <grpc/impl/codegen/grpc_types.h>
#include <grpcpp/server_builder.h>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <thread>
namespace wimi
{
class ImServiceRunner : public Singleton<ImServiceRunner>
{
  private:
    void ClearUp()
    {
        if (cleaned.exchange(true))
            return;

        if (rpcServer)
            rpcServer->Shutdown();
        if (rpcRunThread.joinable()) {
            rpcRunThread.join();
            LOG_INFO(wimi::businessLogger, "[message/shutdown] RPC 服务线程已退出");
        }

        size_t refCount;
        refCount = db::MysqlDao::GetInstance().use_count() - 1;
        db::MysqlDao::GetInstance()->Close();
        if (refCount > 1)
            LOG_ERROR(wimi::dbLogger, "[message/shutdown] MySQL 资源仍被引用, references: {}", refCount);

        refCount = db::RedisDao::GetInstance().use_count() - 1;
        if (refCount > 1)
            LOG_ERROR(wimi::dbLogger, "[message/shutdown] Redis 资源仍被引用, references: {}", refCount);
        db::RedisDao::GetInstance()->Close();
    }

  public:
    bool Activate()
    {
        try {
            auto config = Configer::getNode("server");
            std::string host = config["self"]["host"].as<std::string>();
            std::string rpcPort = config["self"]["rpcPort"].as<std::string>();
            std::string address = host + ":" + rpcPort;
            LOG_INFO(wimi::netLogger, "[message/startup] RPC 服务配置已加载, host: {}, port: {}", host, rpcPort);

            wimi::db::MysqlDao::GetInstance();
            wimi::db::RedisDao::GetInstance();

            // grpc服务
            auto transportSecurity = LoadGrpcSecurityConfig(config);
            rpc::GatewayStreamService gatewayStreamService(config["self"]["name"].as<std::string>(),
                                                           transportSecurity.Mtls());
            Service::GetInstance()->SetGatewayStreamService(&gatewayStreamService);
            grpc::ServerBuilder builder;
            builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
            builder.AddListeningPort(address, BuildServerCredentials(transportSecurity));
            builder.RegisterService(&gatewayStreamService);
            rpcServer = builder.BuildAndStart();
            if (!rpcServer) {
                LOG_ERROR(wimi::netLogger, "[message/startup] RPC 服务启动失败, address: {}", address);
                Service::GetInstance()->SetGatewayStreamService(nullptr);
                ClearUp();
                return false;
            }
            LOG_INFO(wimi::businessLogger, "[message/startup] RPC 服务已启动, address: {}", address);

            rpcRunThread = std::thread([&]() { rpcServer->Wait(); });

            boost::asio::io_context signalIoc;
            boost::asio::signal_set signals(signalIoc, SIGINT, SIGTERM);
            signals.async_wait(
                [&signalIoc, &gatewayStreamService](const boost::system::error_code& error, int signalNumber) {
                    if (error)
                        return;
                    LOG_INFO(wimi::businessLogger, "[message/shutdown] 收到退出信号, signal: {}", signalNumber);
                    gatewayStreamService.DrainAll("message node is shutting down");
                    signalIoc.stop();
                });

            signalIoc.run();
            ClearUp();
            Service::GetInstance()->SetGatewayStreamService(nullptr);

            return true;
        } catch (const std::exception& e) {
            Service::GetInstance()->SetGatewayStreamService(nullptr);
            ClearUp();
            LOG_ERROR(businessLogger, "[message/startup] 服务启动异常, error: {}", e.what());
            return false;
        }
    }

    ImServiceRunner() = default;
    ~ImServiceRunner()
    {
        ClearUp();
        LOG_INFO(businessLogger, "[message/shutdown] 服务已停止");
    }

  private:
    std::unique_ptr<grpc::Server> rpcServer;
    std::thread rpcRunThread;
    std::atomic<bool> cleaned{false};
};
}; // namespace wimi
