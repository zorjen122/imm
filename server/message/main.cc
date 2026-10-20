#include "Configer.h"
#include "Logger.h"
#include "service.hh"
#include <openssl/ssl.h>
#include <seastar/core/app-template.hh>
#include <seastar/core/with_scheduling_group.hh>

namespace
{
seastar::future<> run(seastar::app_template& app)
{
    const char* configured = std::getenv("WIMI_SERVER_LOG_LEVEL");
    const auto level = configured ? std::string(configured) : app.configuration()["log_level"].as<std::string>();
    wimi::Logger::instance().setLevel(spdlog::level::from_str(level));
    wimi::io::initialize_grpc();
    const auto config = wimi::message_io::load_options(Configer::getNode("server"));
    // Seastar 在进程层创建调度组句柄，各 reactor 分别维护该组的执行队列与份额。
    // 因此只创建一次后分发句柄，连接、事务及 IO 状态仍由各 shard 独占。
    const wimi::message::scheduling_groups groups{co_await seastar::create_scheduling_group("protocol", 100),
                                                  co_await seastar::create_scheduling_group("transaction", 100),
                                                  co_await seastar::create_scheduling_group("io", 200)};
    seastar::sharded<wimi::message::shard_service> services;
    bool constructed{};
    std::exception_ptr failure;
    try {
        co_await services.start(config, groups, &services);
        constructed = true;
        co_await services.invoke_on_all([groups](wimi::message::shard_service& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.start(); });
        });
        LOG_INFO(wimi::logger, "[message/startup] ready node={} base_port={} shards={}", config.node_id,
                 config.listener.port, config.shards);
        co_await seastar::engine().wait_for_stop(std::chrono::hours(24 * 365));
    } catch (...) {
        failure = std::current_exception();
    }
    if (constructed) {
        co_await services.invoke_on_all([groups](wimi::message::shard_service& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.stop(); });
        });
        co_await services.stop();
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
    wimi::Logger::instance().initialize(argv[0], wimi::Logger::Service::Message);
    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, nullptr) != 1 || mysql_library_init(0, nullptr, nullptr) != 0) {
        return 1;
    }
    const char* path = std::getenv("WIMI_CONFIG");
    if (!path || !Configer::loadConfig(path)) {
        LOG_ERROR(wimi::logger, "WIMI_CONFIG must name a Message config");
        mysql_library_end();
        return 1;
    }
    seastar::app_template::seastar_options options;
    options.smp_opts.smp.set_default_value(Configer::getNode("server")["self"]["shards"].as<unsigned>(1));
    seastar::app_template app(std::move(options));
    app.add_options()("log_level", boost::program_options::value<std::string>()->default_value("info"),
                      "wimi log level");
    const int status = app.run(argc, argv, [&app] { return run(app); });
    wimi::io::shutdown_grpc();
    mysql_library_end();
    return status;
}
