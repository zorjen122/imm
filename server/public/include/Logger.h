#pragma once

#include "spdlog/common.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <string>
#include <string_view>

namespace wimi
{

inline std::filesystem::path logDirectoryForExecutable(const std::filesystem::path& executable)
{
    if (executable.empty()) {
        return std::filesystem::current_path();
    }

    return std::filesystem::weakly_canonical(executable).parent_path();
}

inline std::string_view logSourceFileName(std::string_view path)
{
    const auto separator = path.find_last_of("/\\");
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

// 日志级别
#define LOG_DEBUG(logger, ...)                                                                                         \
    do {                                                                                                               \
        ::wimi::ensureLoggersInitialized();                                                                            \
        logger->debug("{}:{}({})\n{}", ::wimi::logSourceFileName(__FILE__), __LINE__, __FUNCTION__,                    \
                      fmt::format(__VA_ARGS__));                                                                       \
    } while (false)

#define LOG_TRACE(logger, ...)                                                                                         \
    do {                                                                                                               \
        ::wimi::ensureLoggersInitialized();                                                                            \
        logger->trace("{}", fmt::format(__VA_ARGS__));                                                                 \
    } while (false)

#define LOG_INFO(logger, ...)                                                                                          \
    do {                                                                                                               \
        ::wimi::ensureLoggersInitialized();                                                                            \
        logger->info("{}", fmt::format(__VA_ARGS__));                                                                  \
    } while (false)

#define LOG_WARN(logger, ...)                                                                                          \
    do {                                                                                                               \
        ::wimi::ensureLoggersInitialized();                                                                            \
        logger->warn("{}", fmt::format(__VA_ARGS__));                                                                  \
    } while (false)

#define LOG_ERROR(logger, ...)                                                                                         \
    do {                                                                                                               \
        ::wimi::ensureLoggersInitialized();                                                                            \
        logger->error("{}", fmt::format(__VA_ARGS__));                                                                 \
    } while (false)

inline std::shared_ptr<spdlog::logger> createDebugLogger(std::string name, const std::filesystem::path& path,
                                                         int files = 3,
                                                         spdlog::level::level_enum level = spdlog::level::debug)
{
    // 控制台输出（带颜色）
    auto outSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    outSink->set_level(level);
    outSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");

    std::filesystem::create_directories(path);

    // 文件输出（自动轮转）
    const auto logFile = path / (name + ".log");
    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logFile.string(), 1024 * 1024 * 5,
                                                                           files); // 5MB x 3 files
    fileSink->set_level(level);
    fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");

    auto logger = std::make_shared<spdlog::logger>(name, spdlog::sinks_init_list{outSink, fileSink});
    logger->set_level(level);
    return logger;
}

inline std::once_flag loggerInitializationFlag;
inline std::shared_ptr<spdlog::logger> dbLogger;
inline std::shared_ptr<spdlog::logger> netLogger;
inline std::shared_ptr<spdlog::logger> businessLogger;

inline void initializeLoggers(const std::filesystem::path& executable = {})
{
    std::call_once(loggerInitializationFlag, [&executable] {
        const auto logRoot = logDirectoryForExecutable(executable) / "logs";
        dbLogger = createDebugLogger("db", logRoot / "db", 3, spdlog::level::debug);
        netLogger = createDebugLogger("net", logRoot / "net", 3, spdlog::level::debug);
        businessLogger = createDebugLogger("bussiness", logRoot / "bussiness", 3, spdlog::level::debug);
    });
}

inline void ensureLoggersInitialized() { initializeLoggers(); }

inline void setLoggerLevel(spdlog::level::level_enum level)
{
    ensureLoggersInitialized();
    dbLogger->set_level(level);
    netLogger->set_level(level);
    businessLogger->set_level(level);
}

inline std::string getLogLevelStr()
{
    ensureLoggersInitialized();
    std::string ret{};
    ret += "\n存储日志级别：";
    ret += spdlog::level::to_string_view(dbLogger->level()).data();
    ret += "\n网络日志级别：";
    ret += spdlog::level::to_string_view(netLogger->level()).data();
    ret += "\n业务日志级别：";
    ret += spdlog::level::to_string_view(businessLogger->level()).data();
    ret += "\n";
    return ret;
}

}; // namespace wimi
