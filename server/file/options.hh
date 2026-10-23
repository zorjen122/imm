#pragma once
#include "reactor/types.hh"
#include <yaml-cpp/yaml.h>
#include <cstddef>
#include <filesystem>

namespace imm::file_service
{
struct options {
    io::endpoint listener;
    std::filesystem::path storage_root;
    std::size_t max_requests{64};
    std::size_t max_bytes{64 * 1024 * 1024};
};
options load_options(const YAML::Node& config, const std::filesystem::path& executable);
} // namespace imm::file_service
