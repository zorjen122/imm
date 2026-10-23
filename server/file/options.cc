#include "options.hh"
#include <stdexcept>

namespace imm::file_service
{
options load_options(const YAML::Node& config, const std::filesystem::path& executable)
{
    options result;
    const auto self = config["self"];
    result.listener = {self["host"].as<std::string>(), self["port"].as<uint16_t>()};
    result.storage_root = self["storageRoot"].as<std::string>();
    if (result.storage_root.is_relative()) {
        // 仅在 reactor 启动前解析可执行文件路径；运行期目录操作全部使用异步文件接口。
        result.storage_root = std::filesystem::weakly_canonical(executable).parent_path() / result.storage_root;
    }
    const auto transaction = config["transaction"];
    const auto requests = transaction ? transaction["maxRequests"].as<int64_t>(64) : 64;
    const auto bytes = transaction ? transaction["maxBytes"].as<int64_t>(64 * 1024 * 1024) : 64 * 1024 * 1024;
    if (requests <= 0 || bytes <= 0) {
        throw std::invalid_argument("File transaction.maxRequests and maxBytes must be positive");
    }
    result.max_requests = static_cast<std::size_t>(requests);
    result.max_bytes = static_cast<std::size_t>(bytes);
    return result;
}
} // namespace imm::file_service
