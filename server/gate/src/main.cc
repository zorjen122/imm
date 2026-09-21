#include "Configer.h"
#include "Logger.h"
#include "Mysql.h"
#include "Redis.h"
#include "SeastarExecutor.h"
#include "Service.h"
#include "StateClient.h"

#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/http/httpd.hh>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace
{

constexpr std::size_t kMaxHttpBodyBytes = 64 * 1024;
constexpr std::size_t kMaxHttpPathBytes = 1024;
constexpr std::size_t kMaxBusinessTasks = 128;
constexpr std::size_t kMaxBusinessBytes = 16 * 1024 * 1024;
constexpr std::size_t kBusinessWorkerCount = 4;

void SetLogLevel(const std::string& configured)
{
    const char* level = std::getenv("WIMI_SERVER_LOG_LEVEL");
    const std::string value = level == nullptr ? configured : level;
    if (value == "trace")
        wimi::Logger::instance().setLevel(spdlog::level::trace);
    else if (value == "debug")
        wimi::Logger::instance().setLevel(spdlog::level::debug);
    else if (value == "info")
        wimi::Logger::instance().setLevel(spdlog::level::info);
    else
        throw std::invalid_argument("invalid WIMI_SERVER_LOG_LEVEL");
}

seastar::future<std::unique_ptr<seastar::http::reply>>
HandleHttpRequest(wimi::SeastarExecutor& executor, std::unique_ptr<seastar::http::request> request,
                  std::unique_ptr<seastar::http::reply> reply)
{
    std::string body;
    body.reserve(std::min<std::size_t>(request->content_length, kMaxHttpBodyBytes));
    while (auto chunk = co_await request->content_stream->read()) {
        if (chunk.size() > kMaxHttpBodyBytes - body.size()) {
            reply->set_status(seastar::http::reply::status_type::payload_too_large);
            reply->_content = "request too large";
            co_return reply;
        }
        body.append(chunk.get(), chunk.size());
    }

    std::string path(request->_url);
    if (const auto query = path.find('?'); query != std::string::npos)
        path.resize(query);
    const std::string method(request->_method);
    if (path.size() > kMaxHttpPathBytes) {
        reply->set_status(seastar::http::reply::status_type::uri_too_long);
        co_return reply;
    }

    try {
        auto result = co_await executor.Submit(body.size() + path.size(), [body = std::move(body), path, method] {
            return wimi::Service::GetInstance()->Handle(body, path, method);
        });
        if (result.status == 404)
            LOG_WARN(wimi::logger, "[gate/request] 路由未匹配, method: {}, path: {}", method, path);
        if (result.body.size() > kMaxHttpBodyBytes)
            throw std::length_error("gate response exceeds configured bound");
        reply->set_status(static_cast<seastar::http::reply::status_type>(result.status));
        reply->_content = seastar::sstring(result.body);
        reply->add_header("Content-Type", result.contentType);
    } catch (const wimi::ExecutorOverloaded&) {
        reply->set_status(seastar::http::reply::status_type::service_unavailable);
        reply->_content = "business queue full";
    } catch (const std::exception& error) {
        LOG_ERROR(wimi::logger, "[gate/request] 业务处理失败, error: {}", error.what());
        reply->set_status(seastar::http::reply::status_type::internal_server_error);
        reply->_content = "internal server error";
    }
    co_return reply;
}

seastar::future<> RunGate(seastar::app_template& app)
{
    SetLogLevel(app.configuration()["log_level"].as<std::string>());
    const char* configuredPath = std::getenv("WIMI_CONFIG");
    const std::string configPath = configuredPath == nullptr ? "../conf/gate.yaml" : configuredPath;
    if (!Configer::loadConfig(configPath))
        throw std::runtime_error("failed to load gate config");

    auto config = Configer::getNode("server");
    if (!config || !config["gateway"] || !config["gateway"]["port"])
        throw std::runtime_error("gate listen port is missing");
    const auto port = config["gateway"]["port"].as<unsigned short>();

    wimi::db::MysqlDao::GetInstance();
    wimi::db::RedisDao::GetInstance();
    wimi::rpc::StateClient::GetInstance();
    wimi::Service::GetInstance();

    wimi::SeastarExecutor executor(kBusinessWorkerCount, kMaxBusinessTasks, kMaxBusinessBytes);
    seastar::httpd::http_server_control server;
    co_await server.start("gate");
    co_await server.server().invoke_on_all([](seastar::httpd::http_server& local) {
        local.set_content_length_limit(kMaxHttpBodyBytes);
        local.set_content_streaming(true);
    });
    co_await server.set_routes([&executor](seastar::httpd::routes& routes) {
        for (const auto* path : {"/test", "/post-signIn", "/post-verifycode", "/post-signUp", "/post-forget-password",
                                 "/post-arrhythmia"}) {
            const auto operation = std::string_view(path) == "/test" ? seastar::httpd::operation_type::GET
                                                                     : seastar::httpd::operation_type::POST;
            routes.put(operation, path,
                       new seastar::httpd::function_handler(
                           [&executor](std::unique_ptr<seastar::http::request> request,
                                       std::unique_ptr<seastar::http::reply> reply) {
                               return HandleHttpRequest(executor, std::move(request), std::move(reply));
                           },
                           "json"));
        }
    });
    co_await server.listen(seastar::socket_address(seastar::ipv4_addr(port)));
    LOG_INFO(wimi::logger, "[gate/startup] Seastar HTTP 服务已启动, port: {}", port);

    co_await seastar::engine().wait_for_stop(std::chrono::hours(24 * 365));
    co_await server.stop();
    co_await executor.Stop();
    wimi::db::MysqlDao::GetInstance()->Close();
    wimi::db::RedisDao::GetInstance()->Close();
    LOG_INFO(wimi::logger, "[gate/shutdown] 服务已停止");
}

} // namespace

int main(int argc, char** argv)
{
    wimi::Logger::instance().initialize(argv[0], wimi::Logger::Service::Gate);
    seastar::app_template app;
    namespace bpo = boost::program_options;
    app.add_options()("log_level", bpo::value<std::string>()->default_value("info"), "wimi log level");
    return app.run(argc, argv, [&app] { return RunGate(app); });
}
