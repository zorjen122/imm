#include "configuration.hh"
#include "Logger.h"
#include "reactor/stop_signal.hh"
#include "transport/server.hh"
#include "reactor/grpc_engine.hh"
#include <openssl/ssl.h>
#include <utility>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <optional>
#include <cstdlib>

namespace
{
seastar::future<> run(seastar::app_template& app)
{
    const char* configured = std::getenv("IMM_SERVER_LOG_LEVEL");
    const auto level = configured ? std::string(configured) : app.configuration()["log_level"].as<std::string>();
    imm::Logger::instance().setLevel(spdlog::level::from_str(level));
    imm::io::initialize_grpc();
    const auto config = imm::gateway_io::load_options(imm::configuration::server());
    const imm::gateway_transport::scheduling_groups groups{
        co_await seastar::create_scheduling_group("protocol", 100),
        co_await seastar::create_scheduling_group("transaction", 100),
        co_await seastar::create_scheduling_group("io", 200)};
    seastar::sharded<imm::gateway_transport::server> servers;
    imm::io::stop_signal stop;
    std::optional<seastar::future<>> stopping;
    bool constructed{};
    std::exception_ptr failure;
    try {
        co_await servers.start(config, groups, &servers);
        constructed = true;
        co_await servers.invoke_on_all([groups](imm::gateway_transport::server& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.start(); });
        });
        // 启动等待和正常运行共用退出信号；停止先唤醒各 shard 的就绪等待，再统一清理。
        stopping.emplace(stop.wait().then([&servers] {
            return servers.invoke_on_all([](imm::gateway_transport::server& local) { local.transaction().abort(); });
        }));
        co_await servers.invoke_on_all(
            [](imm::gateway_transport::server& local) { return local.transaction().wait_ready(); });
        co_await servers.invoke_on_all([groups](imm::gateway_transport::server& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.listen(); });
        });
        LOG_INFO(imm::logger, "[gateway/startup] ready gateway={} instance={} port={} shards={}", config.gateway_id,
                 config.instance_id, config.port, seastar::this_smp_shard_count());
        co_await stop.wait();
    } catch (const imm::io::stopped&) {
        if (!stop.stopping()) {
            failure = std::current_exception();
        }
    } catch (...) {
        failure = std::current_exception();
    }
    stop.request_stop();
    if (stopping) {
        co_await std::move(*stopping);
    }
    if (constructed) {
        co_await servers.invoke_on_all([groups](imm::gateway_transport::server& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.stop(); });
        });
        co_await servers.stop();
    }
    co_await imm::io::drain_grpc();
    co_await seastar::destroy_scheduling_group(groups.io);
    co_await seastar::destroy_scheduling_group(groups.transaction);
    co_await seastar::destroy_scheduling_group(groups.protocol);
    if (failure) {
        std::rethrow_exception(failure);
    }
}
} // namespace
int main(int argc, char** argv)
{
    imm::Logger::instance().initialize(argv[0], imm::Logger::Service::Gateway);
    const char* path = std::getenv("IMM_CONFIG");
    if (!path || !imm::configuration::load(path)) {
        LOG_ERROR(imm::logger, "IMM_CONFIG must name a Gateway config");
        return EXIT_FAILURE;
    }

    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, nullptr) != 1 || mysql_library_init(0, nullptr, nullptr) != 0) {
        return 1;
    }

    // SMP 默认值须在 reactor 启动前设置；显式 --smp 仍由 Seastar 优先处理。
    seastar::app_template::seastar_options options;
    options.smp_opts.smp.set_default_value(imm::configuration::server()["self"]["shards"].as<unsigned>(1));
    seastar::app_template app(std::move(options));
    app.add_options()("log_level", boost::program_options::value<std::string>()->default_value("info"),
                      "imm log level");
    const int status = app.run(argc, argv, [&app] { return run(app); });
    imm::io::shutdown_grpc();
    mysql_library_end();
    return status;
}
