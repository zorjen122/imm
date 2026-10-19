#include "Configer.h"
#include "Logger.h"
#include "transport/server.hh"
#include "reactor/grpc_engine.hh"
#include <openssl/ssl.h>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/with_scheduling_group.hh>

namespace
{
seastar::future<> run(seastar::app_template& app)
{
    const char* path = std::getenv("WIMI_CONFIG");
    if (!path || !Configer::loadConfig(path)) {
        throw std::invalid_argument("WIMI_CONFIG must name a Gateway config");
    }
    const char* configured = std::getenv("WIMI_SERVER_LOG_LEVEL");
    const auto level = configured ? std::string(configured) : app.configuration()["log_level"].as<std::string>();
    wimi::Logger::instance().setLevel(spdlog::level::from_str(level));
    wimi::io::initialize_grpc();
    const auto config = wimi::gateway_io::load_options(Configer::getNode("server"));
    const wimi::gateway_transport::scheduling_groups groups{
        co_await seastar::create_scheduling_group("protocol", 100),
        co_await seastar::create_scheduling_group("transaction", 100),
        co_await seastar::create_scheduling_group("io", 200)};
    seastar::sharded<wimi::gateway_transport::server> servers;
    bool constructed{};
    std::exception_ptr failure;
    try {
        co_await servers.start(config, groups, &servers);
        constructed = true;
        co_await servers.invoke_on_all([groups](wimi::gateway_transport::server& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.start(); });
        });
        co_await servers.invoke_on_all([groups](wimi::gateway_transport::server& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.listen(); });
        });
        LOG_INFO(wimi::logger, "[gateway/startup] ready gateway={} instance={} port={} shards={}", config.gateway_id,
                 config.instance_id, config.port, seastar::this_smp_shard_count());
        co_await seastar::engine().wait_for_stop(std::chrono::hours(24 * 365));
    } catch (...) {
        failure = std::current_exception();
    }
    if (constructed) {
        co_await servers.invoke_on_all([groups](wimi::gateway_transport::server& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.stop(); });
        });
        co_await servers.stop();
    }
    co_await wimi::io::drain_grpc();
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
    wimi::Logger::instance().initialize(argv[0], wimi::Logger::Service::Gateway);
    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, nullptr) != 1 || mysql_library_init(0, nullptr, nullptr) != 0) {
        return 1;
    }
    seastar::app_template app;
    app.add_options()("log_level", boost::program_options::value<std::string>()->default_value("info"),
                      "wimi log level");
    const int status = app.run(argc, argv, [&app] { return run(app); });
    wimi::io::shutdown_grpc();
    mysql_library_end();
    return status;
}
