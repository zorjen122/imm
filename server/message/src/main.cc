
#include "ImActiver.h"
#include "Logger.h"
#include <iostream>
#include <spdlog/common.h>
#include <string>

int main(int argc, char* argv[])
{
    wimi::initializeLoggers(argv[0]);
    if (argc < 2 || argc > 3) {
        std::cout << "Usage: " << argv[0] << " [config file] [--debug | --info | --trace]\n";
        return -1;
    }

    auto loadSuccess = Configer::loadConfig(argv[1]);
    auto node = Configer::getNode("server");
    if (!loadSuccess || node.IsNull()) {
        LOG_ERROR(wimi::businessLogger, "[message/startup] 配置加载失败或缺少 server 配置");
        return -1;
    }

    if (argc == 3) {
        auto level = std::string(argv[2]);
        if (level == "--debug") {
            wimi::setLoggerLevel(spdlog::level::debug);
        } else if (level == "--info") {
            wimi::setLoggerLevel(spdlog::level::info);
        } else if (level == "--trace") {
            wimi::setLoggerLevel(spdlog::level::trace);
        } else {
            std::cout << "Invalid log level: " << level << ". Valid options: --debug, --info, --trace\n";
            return -1;
        }
    }

    LOG_INFO(wimi::businessLogger, "[message/startup] 正在启动服务, log_level: {}",
             spdlog::level::to_string_view(wimi::businessLogger->level()));
    wimi::ImServiceRunner::GetInstance()->Activate();

    return 0;
}
