#include "Configer.h"
#include "GatewayAcceptor.h"
#include "Logger.h"
#include "MessageLinkManager.h"
#include "Mysql.h"
#include "Redis.h"
#include "SessionManager.h"

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

using wimi::Logger;
using wimi::logger;

namespace
{

constexpr std::size_t kBusinessWorkerCount = 4;
constexpr std::size_t kMinIoWorkerCount = 2;
constexpr std::size_t kMaxIoWorkerCount = 8;

} // namespace

int main(int argc, char** argv)
{
    Logger::instance().initialize(argv[0], Logger::Service::Gateway);
    const char* configPath = std::getenv("WIMI_CONFIG");
    const char* logLevel = std::getenv("WIMI_SERVER_LOG_LEVEL");
    auto logLevelArgument = 1;
    if (configPath == nullptr && argc > 1) {
        configPath = argv[1];
        logLevelArgument = 2;
    }
    if (argc == logLevelArgument + 2 && std::string_view(argv[logLevelArgument]) == "--log_level") {
        if (logLevel == nullptr)
            logLevel = argv[logLevelArgument + 1];
    } else if (argc != logLevelArgument) {
        std::cerr << "Usage: " << argv[0] << " [config file] [--log_level <debug|info|trace>]\n";
        return EXIT_FAILURE;
    }
    if (logLevel != nullptr) {
        if (std::string_view(logLevel) == "debug")
            Logger::instance().setLevel(spdlog::level::debug);
        else if (std::string_view(logLevel) == "info")
            Logger::instance().setLevel(spdlog::level::info);
        else if (std::string_view(logLevel) == "trace")
            Logger::instance().setLevel(spdlog::level::trace);
        else {
            std::cerr << "Invalid log level: " << logLevel << '\n';
            return EXIT_FAILURE;
        }
    }

    if (configPath == nullptr) {
        std::cerr << "Gateway config path is required.\n";
        return EXIT_FAILURE;
    }
    if (!Configer::loadConfig(configPath)) {
        LOG_ERROR(logger, "[gateway/startup] 配置加载失败");
        return EXIT_FAILURE;
    }

    auto config = Configer::getNode("server");
    if (!config || !config["self"] || !config["self"]["name"] || !config["self"]["port"]) {
        LOG_ERROR(logger, "[gateway/startup] 缺少服务名称或监听端口配置");
        return EXIT_FAILURE;
    }

    const std::string gatewayId = config["self"]["name"].as<std::string>();
    const unsigned short port = config["self"]["port"].as<unsigned short>();
    const std::string instanceId = boost::uuids::to_string(boost::uuids::random_generator{}());

    wimi::db::MysqlDao::GetInstance();
    wimi::db::RedisDao::GetInstance();

    boost::asio::io_context ioContext;
    boost::asio::thread_pool businessPool(kBusinessWorkerCount);
    wimi::connection::SessionManager sessions(gatewayId, instanceId);
    wimi::connection::MessageLinkManager messageLinks(ioContext, businessPool, gatewayId, instanceId);
    messageLinks.SetClientForwardHandler(
        [&sessions](const wimi::gateway::ClientForwardEnvelope& forward) { return sessions.Forward(forward); });
    messageLinks.Start();

    wimi::connection::GatewayAcceptor server(ioContext, port, sessions, messageLinks, businessPool);
    boost::asio::co_spawn(ioContext, server.Run(), boost::asio::detached);

    boost::asio::signal_set signals(ioContext, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& error, int signalNumber) {
        if (error)
            return;
        LOG_INFO(logger, "[gateway/shutdown] 收到退出信号, signal: {}", signalNumber);
        messageLinks.Stop();
        ioContext.stop();
    });

    const auto workerCount =
        std::clamp<std::size_t>(std::thread::hardware_concurrency(), kMinIoWorkerCount, kMaxIoWorkerCount);
    std::vector<std::thread> workers;
    workers.reserve(workerCount - 1);
    for (std::size_t i = 1; i < workerCount; ++i)
        workers.emplace_back([&]() { ioContext.run(); });
    LOG_INFO(logger, "[gateway/startup] 服务已启动, gateway: {}, instance: {}, port: {}", gatewayId, instanceId, port);
    ioContext.run();
    for (auto& worker : workers)
        worker.join();

    messageLinks.Stop();
    businessPool.stop();
    businessPool.join();
    wimi::db::MysqlDao::GetInstance()->Close();
    wimi::db::RedisDao::GetInstance()->Close();
    LOG_INFO(logger, "[gateway/shutdown] 服务已停止, gateway: {}, instance: {}", gatewayId, instanceId);
    return EXIT_SUCCESS;
}
