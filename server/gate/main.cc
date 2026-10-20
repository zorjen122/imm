#include "configuration.hh"
#include "Logger.h"
#include "api/protocol.hh"
#include "reactor/grpc_engine.hh"
#include <seastar/core/sharded.hh>
#include <openssl/ssl.h>

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
void set_log_level(const std::string& configured)
{
    const char* level = std::getenv("IMM_SERVER_LOG_LEVEL");
    const std::string value = level == nullptr ? configured : level;
    if (value == "trace")
        imm::Logger::instance().setLevel(spdlog::level::trace);
    else if (value == "debug")
        imm::Logger::instance().setLevel(spdlog::level::debug);
    else if (value == "info")
        imm::Logger::instance().setLevel(spdlog::level::info);
    else
        throw std::invalid_argument("invalid IMM_SERVER_LOG_LEVEL");
}

seastar::future<std::unique_ptr<seastar::http::reply>>
handle_http_request(seastar::sharded<imm::gate_transaction::service>& services,
                    const imm::gate_protocol::route& route_endpoint, imm::gate_protocol::scheduling_groups groups,
                    std::unique_ptr<seastar::http::request> request, std::unique_ptr<seastar::http::reply> reply)
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

    try {
        auto result = co_await imm::gate_protocol::dispatch(route_endpoint, services.local(), std::move(body),
                                                            groups.transaction);
        if (result.body.size() > kMaxHttpBodyBytes) {
            throw std::length_error("gate response exceeds configured bound");
        }
        reply->set_status(static_cast<seastar::http::reply::status_type>(result.status));
        reply->_content = seastar::sstring(result.body);
        reply->add_header("Content-Type", "application/json");
    } catch (const std::exception& error) {
        LOG_ERROR(imm::logger, "[gate/request] 业务处理失败, error: {}", error.what());
        reply->set_status(seastar::http::reply::status_type::internal_server_error);
        reply->_content = "internal server error";
    }
    co_return reply;
}

seastar::future<> run_gate(seastar::app_template& app)
{
    set_log_level(app.configuration()["log_level"].as<std::string>());
    const char* configuredPath = std::getenv("IMM_CONFIG");
    const std::string configPath = configuredPath == nullptr ? "loopback/single.yaml" : configuredPath;
    if (!imm::configuration::load(configPath)) {
        throw std::runtime_error("failed to load gate config");
    }

    auto config = imm::configuration::server();
    if (!config || !config["self"] || !config["self"]["port"]) {
        throw std::runtime_error("gate listen port is missing");
    }
    const auto port = config["self"]["port"].as<unsigned short>();

    const auto io_options = imm::gate_io::load_options(config);

    imm::io::initialize_grpc();
    const imm::gate_protocol::scheduling_groups groups{co_await seastar::create_scheduling_group("protocol", 100),
                                                       co_await seastar::create_scheduling_group("transaction", 100),
                                                       co_await seastar::create_scheduling_group("io", 200)};
    seastar::sharded<imm::gate_transaction::service> services;
    bool services_constructed{};
    std::exception_ptr failure;
    seastar::httpd::http_server_control server;
    try {
        co_await services.start(io_options, groups.transaction, groups.io);
        services_constructed = true;
        co_await services.invoke_on_all([groups](imm::gate_transaction::service& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.start(); });
        });
        co_await server.start("gate");
        co_await server.server().invoke_on_all([groups](seastar::httpd::http_server& local) {
            local.set_request_scheduling_group(groups.protocol);
            local.set_content_length_limit(kMaxHttpBodyBytes);
            local.set_content_streaming(true);
        });
        co_await server.set_routes([&services, groups](seastar::httpd::routes& routes) {
            for (const auto& route_endpoint : imm::gate_protocol::routes()) {
                const auto operation = route_endpoint.method == "GET" ? seastar::httpd::operation_type::GET
                                                                      : seastar::httpd::operation_type::POST;
                routes.put(operation, seastar::sstring(route_endpoint.path),
                           new seastar::httpd::function_handler(
                               [&services, groups, route_endpoint](std::unique_ptr<seastar::http::request> request,
                                                                   std::unique_ptr<seastar::http::reply> reply) {
                                   return handle_http_request(services, route_endpoint, groups, std::move(request),
                                                              std::move(reply));
                               },
                               "json"));
            }
        });
        co_await server.listen(seastar::socket_address(seastar::ipv4_addr(port)));
        LOG_INFO(imm::logger, "[gate/startup] Seastar HTTP 服务已启动, port: {}", port);

        co_await seastar::engine().wait_for_stop(std::chrono::hours(24 * 365));
    } catch (...) {
        failure = std::current_exception();
    }
    // 先让各 shard 拒绝业务并取消依赖，等待业务/驱动退出；再停止 HTTP 服务，
    // 销毁 shard 实例。调度组最后释放，因为在途协程恢复时仍会使用原组。
    if (services_constructed) {
        co_await services.invoke_on_all([groups](imm::gate_transaction::service& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.stop(); });
        });
    }
    co_await server.stop();
    if (services_constructed) {
        co_await services.stop();
    }
    co_await imm::io::drain_grpc();
    co_await seastar::destroy_scheduling_group(groups.io);
    co_await seastar::destroy_scheduling_group(groups.transaction);
    co_await seastar::destroy_scheduling_group(groups.protocol);
    LOG_INFO(imm::logger, "[gate/shutdown] 服务已停止");
    if (failure) {
        std::rethrow_exception(failure);
    }
}

} // namespace

int main(int argc, char** argv)
{
    imm::Logger::instance().initialize(argv[0], imm::Logger::Service::Gate);
    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, nullptr) != 1 || mysql_library_init(0, nullptr, nullptr) != 0) {
        return EXIT_FAILURE;
    }
    seastar::app_template app;
    namespace bpo = boost::program_options;
    app.add_options()("log_level", bpo::value<std::string>()->default_value("info"), "imm log level");
    const int result = app.run(argc, argv, [&app] { return run_gate(app); });
    imm::io::shutdown_grpc();
    mysql_library_end();
    return result;
}
