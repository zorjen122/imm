#pragma once
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

class Configer
{
  public:
    static std::unordered_map<std::string, YAML::Node> configMap;
    static bool loadConfig(const std::string& configFile);
    static YAML::Node getNode(const std::string& filed);
}; // namespace Configer
