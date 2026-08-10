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
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    wimi::initializeLoggers(argv[0]);
    const char* configPath = std::getenv("WIMI_CONFIG");
    if (!configPath && argc > 1)
        configPath = argv[1];
    if (!Configer::loadConfig(configPath ? configPath : "../conf/gateway-hunan.yaml")) {
        spdlog::error("Connection Gateway config load failed");
        return EXIT_FAILURE;
    }

    auto config = Configer::getNode("server");
    if (!config || !config["self"] || !config["self"]["name"] || !config["self"]["port"]) {
        spdlog::error("Connection Gateway self config is missing");
        return EXIT_FAILURE;
    }

    const std::string gatewayId = config["self"]["name"].as<std::string>();
    const unsigned short port = config["self"]["port"].as<unsigned short>();
    const std::string instanceId = boost::uuids::to_string(boost::uuids::random_generator{}());

    wimi::db::MysqlDao::GetInstance();
    wimi::db::RedisDao::GetInstance();

    boost::asio::io_context ioContext;
    boost::asio::thread_pool businessPool(4);
    wimi::connection::SessionManager sessions(gatewayId, instanceId);
    wimi::connection::MessageLinkManager messageLinks(ioContext, businessPool, gatewayId, instanceId);
    messageLinks.SetClientForwardHandler(
        [&sessions](const wimi::gateway::ClientForwardEnvelope& forward) { return sessions.Forward(forward); });
    messageLinks.Start();

    wimi::connection::GatewayAcceptor server(ioContext, port, sessions, messageLinks, businessPool);
    boost::asio::co_spawn(ioContext, server.Run(), boost::asio::detached);

    boost::asio::signal_set signals(ioContext, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code& error, int) {
        if (error)
            return;
        messageLinks.Stop();
        ioContext.stop();
    });

    const auto workerCount = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 2, 8);
    std::vector<std::thread> workers;
    workers.reserve(workerCount - 1);
    for (std::size_t i = 1; i < workerCount; ++i)
        workers.emplace_back([&]() { ioContext.run(); });
    LOG_INFO(wimi::businessLogger, "Connection Gateway started, id: {}, instance: {}, port: {}", gatewayId, instanceId,
             port);
    ioContext.run();
    for (auto& worker : workers)
        worker.join();

    messageLinks.Stop();
    businessPool.stop();
    businessPool.join();
    wimi::db::MysqlDao::GetInstance()->Close();
    wimi::db::RedisDao::GetInstance()->Close();
    return EXIT_SUCCESS;
}
