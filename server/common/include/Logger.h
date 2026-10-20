#pragma once

#include "spdlog/common.h"
#include "spdlog/spdlog.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <string>
#include <string_view>

namespace imm
{

inline std::string_view logSourceFileName(std::string_view path)
{
    const auto separator = path.find_last_of("/\\");
    return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

#define LOG_DEBUG(logger, ...)                                                                                         \
    (spdlog::default_logger_raw()->debug("{}:{}({})\n{}", ::imm::logSourceFileName(__FILE__), __LINE__, __FUNCTION__,  \
                                         fmt::format(__VA_ARGS__)))

#define LOG_TRACE(logger, ...) (spdlog::default_logger_raw()->trace("{}", fmt::format(__VA_ARGS__)))

#define LOG_INFO(logger, ...) (spdlog::default_logger_raw()->info("{}", fmt::format(__VA_ARGS__)))

#define LOG_WARN(logger, ...) (spdlog::default_logger_raw()->warn("{}", fmt::format(__VA_ARGS__)))

#define LOG_ERROR(logger, ...) (spdlog::default_logger_raw()->error("{}", fmt::format(__VA_ARGS__)))

class Logger final
{
  public:
    enum class Service {
        File,
        Gate,
        Gateway,
        Message,
        State,
    };

    static Logger& instance()
    {
        static Logger logger;
        return logger;
    }

    void initialize(const std::filesystem::path& executable, Service service)
    {
        std::call_once(initializationFlag, [this, &executable, service] {
            const std::string name{serviceName(service)};
            const auto logRoot = std::filesystem::weakly_canonical(executable).parent_path() / "logs";
            serviceLogger_ = createLogger(name, logRoot / name);
            spdlog::set_default_logger(serviceLogger_);
        });
    }

    void setLevel(spdlog::level::level_enum level)
    {
        spdlog::set_level(level);
        serviceLogger_->set_level(level);
    }

    std::string levelString()
    {
        std::string result{};
        result += "\n服务日志级别：";
        result += spdlog::level::to_string_view(serviceLogger_->level()).data();
        result += "\n";
        return result;
    }

    std::shared_ptr<spdlog::logger>& logger() { return serviceLogger_; }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

  private:
    Logger() = default;

    static std::string_view serviceName(Service service)
    {
        switch (service) {
        case Service::File:
            return "file";
        case Service::Gate:
            return "gate";
        case Service::Gateway:
            return "gateway";
        case Service::Message:
            return "message";
        case Service::State:
            return "state";
        }
    }

    static std::shared_ptr<spdlog::logger> createLogger(const std::string& name, const std::filesystem::path& path)
    {
        auto outSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        outSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");

        std::filesystem::create_directories(path);
        auto fileSink =
            std::make_shared<spdlog::sinks::daily_file_sink_mt>((path / (name + ".log")).string(), 0, 0, false, 15);
        fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [thread %t] %v");

        auto logger = std::make_shared<spdlog::logger>(name, spdlog::sinks_init_list{outSink, fileSink});
        return logger;
    }

    std::once_flag initializationFlag;
    std::shared_ptr<spdlog::logger> serviceLogger_;
};

inline auto& logger = Logger::instance().logger();

}; // namespace imm
