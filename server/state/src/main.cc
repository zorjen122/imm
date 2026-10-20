#include "configuration.hh"
#include "Logger.h"
#include "service.h"
#include <cstdlib>
#include <grpcpp/server_builder.h>
#include <spdlog/spdlog.h>
#include <string_view>
/*
 * 注意：该状态服务只是一个微服务，运行在网关服务器主机，其目的仅在后续扩展以及独立网关服务器的路由职责。[2025-3-18]
 */
using imm::Logger;

int main(int argc, char* argv[])
{
    Logger::instance().initialize(argv[0], Logger::Service::State);
    const char* logLevel = std::getenv("IMM_SERVER_LOG_LEVEL");
    if (argc == 3 && std::string_view(argv[1]) == "--log_level") {
        if (logLevel == nullptr)
            logLevel = argv[2];
    } else if (argc != 1) {
        return EXIT_FAILURE;
    }
    if (logLevel != nullptr) {
        if (std::string_view(logLevel) == "debug")
            Logger::instance().setLevel(spdlog::level::debug);
        else if (std::string_view(logLevel) == "info")
            Logger::instance().setLevel(spdlog::level::info);
        else if (std::string_view(logLevel) == "trace")
            Logger::instance().setLevel(spdlog::level::trace);
        else
            return EXIT_FAILURE;
    }
    const char* configPath = std::getenv("IMM_CONFIG");
    auto existConfig = imm::configuration::load(configPath ? configPath : "loopback/single.yaml");
    if (!existConfig) {
        spdlog::error("Config load failed");
        return 0;
    }

    auto config = imm::configuration::server();
    if (!config || !config["self"]) {
        spdlog::error("self config not found");
        return 0;
    }

    auto host = config["self"]["host"].as<std::string>();
    auto port = config["self"]["port"].as<std::string>();

    imm::rpc::StateServiceImpl stateImpl;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(host + ":" + port, grpc::InsecureServerCredentials());
    builder.RegisterService(&stateImpl);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());

    spdlog::info("State Service on startting, listening on {}:{}", host, port);
    server->Wait();

    return 0;
}
