
#include "ImActiver.h"
#include "Logger.h"
#include <cstdlib>
#include <iostream>
#include <spdlog/common.h>
#include <string>
#include <string_view>

using wimi::Logger;
using wimi::logger;

int main(int argc, char* argv[])
{
    Logger::instance().initialize(argv[0], Logger::Service::Message);
    const char* logLevel = std::getenv("WIMI_SERVER_LOG_LEVEL");
    if (argc == 4 && std::string_view(argv[2]) == "--log_level") {
        if (logLevel == nullptr)
            logLevel = argv[3];
    } else if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " <config file> [--log_level <debug|info|trace>]\n";
        return -1;
    }
    if (logLevel != nullptr) {
        if (std::string_view(logLevel) == "debug")
            Logger::instance().setLevel(spdlog::level::debug);
        else if (std::string_view(logLevel) == "info")
            Logger::instance().setLevel(spdlog::level::info);
        else if (std::string_view(logLevel) == "trace")
            Logger::instance().setLevel(spdlog::level::trace);
        else {
            std::cout << "Invalid log level: " << logLevel << '\n';
            return -1;
        }
    }

    auto loadSuccess = Configer::loadConfig(argv[1]);
    auto node = Configer::getNode("server");
    if (!loadSuccess || node.IsNull()) {
        LOG_ERROR(logger, "[message/startup] 配置加载失败或缺少 server 配置");
        return -1;
    }

    LOG_INFO(logger, "[message/startup] 正在启动服务, log_level: {}", spdlog::level::to_string_view(logger->level()));
    wimi::ImServiceRunner::GetInstance()->Activate();

    return 0;
}
