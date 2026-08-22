
#include "Configer.h"
#include "GateServer.h"
#include "IocPool.h"
#include "Logger.h"
#include "StateClient.h"

#include "Mysql.h"
#include "Redis.h"
#include <iostream>
#include <cstdlib>
#include <jsoncpp/json/json.h>
#include <jsoncpp/json/reader.h>
#include <jsoncpp/json/value.h>
#include <string_view>

using wimi::Logger;

int main(int argc, char* argv[])
{
    Logger::instance().initialize(argv[0], Logger::Service::Gate);
    const char* logLevel = std::getenv("WIMI_SERVER_LOG_LEVEL");
    if (argc == 3 && std::string_view(argv[1]) == "--log_level") {
        if (logLevel == nullptr)
            logLevel = argv[2];
    } else if (argc != 1) {
        std::cerr << "Usage: " << argv[0] << " [--log_level <debug|info|trace>]\n";
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
    try {
        const char* configPath = std::getenv("WIMI_CONFIG");
        auto existConfig = Configer::loadConfig(configPath ? configPath : "../conf/gate.yaml");
        if (!existConfig) {
            spdlog::error("Config load failed");
            return 0;
        }

        auto config = Configer::getNode("server");
        if (!config || !config["gateway"]) {
            spdlog::error("gateway config not found");
            return 0;
        }

        wimi::IocPool::GetInstance();
        wimi::db::MysqlDao::GetInstance();
        wimi::db::RedisDao::GetInstance();
        wimi::rpc::StateClient::GetInstance();

        unsigned short port = config["gateway"]["port"].as<unsigned short>();

        net::io_context ioc;
        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&ioc](const boost::system::error_code& error, int signal_number) {
            if (error)
                return;

            ioc.stop();
        });

        auto gate = std::make_shared<wimi::GateServer>(ioc, port);
        gate->Start();
        std::cout << "Gate Server listen on port: " << port << "\n";

        ioc.run();
    } catch (std::exception const& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
