#include "configuration.hh"
#include "Logger.h"
#include "reactor/stop_signal.hh"
#include "service.hh"
#include <openssl/ssl.h>
#include <seastar/core/app-template.hh>
#include <seastar/core/with_scheduling_group.hh>

namespace
{
seastar::future<> run(seastar::app_template& app)
{
    const char* configured = std::getenv("IMM_SERVER_LOG_LEVEL");
    const auto level = configured ? std::string(configured) : app.configuration()["log_level"].as<std::string>();
    imm::Logger::instance().setLevel(spdlog::level::from_str(level));
    imm::io::initialize_grpc();
    const auto config = imm::message_io::load_options(imm::configuration::server());
    // Seastar 在进程层创建调度组句柄，各 reactor 分别维护该组的执行队列与份额。
    // 因此只创建一次后分发句柄，连接、事务及 IO 状态仍由各 shard 独占。
    const imm::message::scheduling_groups groups{co_await seastar::create_scheduling_group("protocol", 100),
                                                 co_await seastar::create_scheduling_group("transaction", 100),
                                                 co_await seastar::create_scheduling_group("io", 200)};
    seastar::sharded<imm::message::shard_service> services;
    imm::io::stop_signal stop;
    bool constructed{};
    std::exception_ptr failure;
    try {
        co_await services.start(config, groups, &services);
        constructed = true;
        co_await services.invoke_on_all([groups](imm::message::shard_service& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.start(); });
        });
        LOG_INFO(imm::logger, "[message/startup] ready node={} base_port={} shards={}", config.node_id,
                 config.listener.port, config.shards);
        co_await stop.wait();
    } catch (...) {
        failure = std::current_exception();
    }
    if (constructed) {
        co_await services.invoke_on_all([groups](imm::message::shard_service& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.stop(); });
        });
        co_await services.stop();
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
    imm::Logger::instance().initialize(argv[0], imm::Logger::Service::Message);
    if (OPENSSL_init_ssl(OPENSSL_INIT_LOAD_CONFIG, nullptr) != 1 || mysql_library_init(0, nullptr, nullptr) != 0) {
        return 1;
    }
    const char* path = std::getenv("IMM_CONFIG");
    if (!path || !imm::configuration::load(path)) {
        LOG_ERROR(imm::logger, "IMM_CONFIG must name a Message config");
        mysql_library_end();
        return 1;
    }
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
