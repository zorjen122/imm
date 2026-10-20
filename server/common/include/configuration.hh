#pragma once
#include <string>
#include <yaml-cpp/yaml.h>

namespace imm::configuration
{
// 在开始接收请求前加载。extra 只允许追加具名节点，不覆盖 shared 或既有节点。
// 返回当前节点配置及由同一节点表解析的依赖端点和 State 拓扑。
YAML::Node select(const YAML::Node& base, const YAML::Node& extra, const std::string& node_id);
bool load(const std::string& path);
YAML::Node server();
} // namespace imm::configuration
