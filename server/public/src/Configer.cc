#include "Configer.h"
#include "Logger.h"
#include <exception>

std::unordered_map<std::string, YAML::Node> Configer::configMap = {};

bool Configer::loadConfig(const std::string& configFile)
{
    try {
        YAML::Node config = YAML::LoadFile(configFile);
        if (config.IsNull()) {
            LOG_WARN(wimi::businessLogger, "无效配置文件，文件名: {}", configFile);
            return false;
        }

        for (auto it = config.begin(); it != config.end(); ++it) {
            configMap[it->first.as<std::string>()] = it->second;
        }

        LOG_INFO(wimi::businessLogger, "配置文件加载成功，文件名: {}", configFile);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Error load config file: {}", e.what());
        return false;
    }
}

YAML::Node Configer::getNode(const std::string& filed)
{
    auto it = configMap.find(filed);
    if (it == configMap.end()) {
        LOG_DEBUG(wimi::businessLogger, "配置文件中没有这样的字段: {}", filed);
        return YAML::Node();
    }

    return it->second;
}
