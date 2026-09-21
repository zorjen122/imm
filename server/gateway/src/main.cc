#include "Configer.h"
#include "Logger.h"
#include "MessageLinkManager.h"
#include "Mysql.h"
#include "Redis.h"
#include "SeastarExecutor.h"
#include "SeastarGateway.h"
#include "SessionManager.h"

#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/sleep.hh>

#include <boost/asio.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr std::size_t kControlWorkerCount = 4;
constexpr std::size_t kMessageIoWorkerCount = 2;
constexpr std::size_t kBusinessWorkerCount = 4;
constexpr std::size_t kMaxBusinessTasks = 512;
constexpr std::size_t kMaxBusinessBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxProcessQueuedWrites = 65536;
constexpr std::size_t kMaxProcessQueuedWriteBytes = 512 * 1024 * 1024;
constexpr std::size_t kMaxProcessReliableWrites = 32768;
constexpr std::size_t kMaxProcessReliableWriteBytes = 256 * 1024 * 1024;

void SetLogLevel(const char* environment, const std::string& argument)
{
    const std::string level = environment == nullptr ? argument : environment;
    if (level.empty() || level == "info")
        wimi::Logger::instance().setLevel(spdlog::level::info);
    else if (level == "debug")
        wimi::Logger::instance().setLevel(spdlog::level::debug);
    else if (level == "trace")
        wimi::Logger::instance().setLevel(spdlog::level::trace);
    else
        throw std::invalid_argument("invalid gateway log level");
}

seastar::future<> RunGateway(seastar::app_template& app, wimi::SeastarExecutor& executor,
                             wimi::connection::SessionManager& sessions,
                             wimi::connection::MessageLinkManager& messageLinks,
                             wimi::connection::GatewayQueueBudget& queueBudget, std::string gatewayId,
                             std::string instanceId, unsigned short port)
{
    SetLogLevel(std::getenv("WIMI_SERVER_LOG_LEVEL"), app.configuration()["log_level"].as<std::string>());

    const auto readinessDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!messageLinks.Ready()) {
        if (std::chrono::steady_clock::now() >= readinessDeadline)
            throw std::runtime_error("message links did not become ready within 30 seconds");
        co_await seastar::sleep(std::chrono::milliseconds(100));
    }

    seastar::sharded<wimi::connection::SeastarGatewayServer> servers;
    co_await servers.start(std::ref(executor), std::ref(sessions), std::ref(messageLinks), std::ref(queueBudget),
                           &servers, port);
    co_await servers.invoke_on_all(&wimi::connection::SeastarGatewayServer::Start);
    LOG_INFO(wimi::logger,
             "[gateway/startup] Seastar LV TCP 服务已启动, gateway: {}, instance: {}, port: {}, shards: {}", gatewayId,
             instanceId, port, seastar::this_smp_shard_count());

    co_await seastar::engine().wait_for_stop(std::chrono::hours(24 * 365));
    co_await servers.invoke_on_all(&wimi::connection::SeastarGatewayServer::Stop);
    messageLinks.Stop();
    co_await servers.stop();
    co_await executor.Stop();
    LOG_INFO(wimi::logger, "[gateway/shutdown] Seastar LV TCP 服务已停止, gateway: {}, instance: {}", gatewayId,
             instanceId);
}

} // namespace

int main(int argc, char** argv)
{
    wimi::Logger::instance().initialize(argv[0], wimi::Logger::Service::Gateway);

    const char* configuredPath = std::getenv("WIMI_CONFIG");
    if (configuredPath == nullptr || !Configer::loadConfig(configuredPath)) {
        std::cerr << "Gateway config path is required through WIMI_CONFIG.\n";
        return EXIT_FAILURE;
    }
    auto config = Configer::getNode("server");
    if (!config || !config["self"] || !config["self"]["name"] || !config["self"]["port"]) {
        std::cerr << "Gateway service name or listen port is missing.\n";
        return EXIT_FAILURE;
    }

    const std::string gatewayId = config["self"]["name"].as<std::string>();
    const auto port = config["self"]["port"].as<unsigned short>();
    const std::string instanceId = boost::uuids::to_string(boost::uuids::random_generator{}());

    wimi::db::MysqlDao::GetInstance();
    wimi::db::RedisDao::GetInstance();

    boost::asio::io_context messageIo;
    auto messageWork = boost::asio::make_work_guard(messageIo);
    boost::asio::thread_pool controlPool(kControlWorkerCount);
    wimi::connection::SessionManager sessions(gatewayId, instanceId);
    wimi::connection::MessageLinkManager messageLinks(messageIo, controlPool, gatewayId, instanceId);
    messageLinks.SetClientForwardHandler(
        [&sessions](const wimi::gateway::ClientForwardEnvelope& forward) { return sessions.Forward(forward); });
    messageLinks.Start();

    std::vector<std::thread> messageWorkers;
    messageWorkers.reserve(kMessageIoWorkerCount);
    for (std::size_t i = 0; i < kMessageIoWorkerCount; ++i)
        messageWorkers.emplace_back([&messageIo] { messageIo.run(); });

    wimi::SeastarExecutor executor(kBusinessWorkerCount, kMaxBusinessTasks, kMaxBusinessBytes);
    wimi::connection::GatewayQueueBudget queueBudget(kMaxProcessQueuedWrites, kMaxProcessQueuedWriteBytes,
                                                     kMaxProcessReliableWrites, kMaxProcessReliableWriteBytes);
    seastar::app_template app;
    namespace bpo = boost::program_options;
    app.add_options()("log_level", bpo::value<std::string>()->default_value("info"), "wimi log level");

    int exitCode = EXIT_FAILURE;
    try {
        exitCode = app.run(argc, argv, [&] {
            return RunGateway(app, executor, sessions, messageLinks, queueBudget, gatewayId, instanceId, port);
        });
    } catch (const std::exception& error) {
        std::cerr << "Gateway failed: " << error.what() << '\n';
    }

    messageLinks.Stop();
    messageWork.reset();
    messageIo.stop();
    for (auto& worker : messageWorkers)
        worker.join();
    controlPool.stop();
    controlPool.join();
    wimi::db::MysqlDao::GetInstance()->Close();
    wimi::db::RedisDao::GetInstance()->Close();
    return exitCode;
}
