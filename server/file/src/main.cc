#include "Configer.h"
#include "Logger.h"
#include "Service.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

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

using wimi::Logger;

int main(int argc, char* argv[])
{
    Logger::instance().initialize(argv[0], Logger::Service::File);
    const char* configPath = std::getenv("WIMI_CONFIG");
    const char* logLevel = std::getenv("WIMI_SERVER_LOG_LEVEL");
    auto logLevelArgument = 1;
    if (configPath == nullptr && argc > 1) {
        configPath = argv[1];
        logLevelArgument = 2;
    }
    if (argc == logLevelArgument + 2 && std::string_view(argv[logLevelArgument]) == "--log_level") {
        if (logLevel == nullptr)
            logLevel = argv[logLevelArgument + 1];
    } else if (argc != logLevelArgument) {
        std::cerr << "Usage: " << argv[0] << " [config file] [--log_level <debug|info|trace>]\n";
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
