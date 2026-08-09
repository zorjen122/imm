#include "Configer.h"
#include "Service.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <string>

namespace
{
std::filesystem::path ResolveStorageRoot(const std::filesystem::path& configuredRoot, const char* executable)
{
    if (configuredRoot.is_absolute()) {
        return configuredRoot;
    }

    const auto executablePath = std::filesystem::weakly_canonical(executable);
    return executablePath.parent_path() / configuredRoot;
}
} // namespace

int main(int argc, char* argv[])
{
    const char* configPath = std::getenv("WIMI_CONFIG");
    if (configPath == nullptr && argc > 1) {
        configPath = argv[1];
    }
    if (!Configer::loadConfig(configPath == nullptr ? "../conf/file.yaml" : configPath)) {
        spdlog::error("File service config load failed");
        return EXIT_FAILURE;
    }

    const auto server = Configer::getNode("server");
    if (!server || !server["file"] || !server["file"]["host"] || !server["file"]["rpcPort"] ||
        !server["file"]["storageRoot"]) {
        spdlog::error("File service config requires server.file.host, rpcPort and storageRoot");
        return EXIT_FAILURE;
    }

    try {
        const auto file = server["file"];
        const std::string host = file["host"].as<std::string>();
        const auto port = file["rpcPort"].as<unsigned short>();
        const std::filesystem::path configuredRoot = file["storageRoot"].as<std::string>();
        const std::filesystem::path root = ResolveStorageRoot(configuredRoot, argv[0]);
        spdlog::info("File service storage root: {}", root.string());
        wimi::rpc::FileServer fileServer(2, root);
        fileServer.Run(host, port);
    } catch (const std::exception& error) {
        spdlog::error("File service config is invalid: {}", error.what());
        return EXIT_FAILURE;
    }

    return 0;
}
