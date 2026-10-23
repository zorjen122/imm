#include "configuration.hh"
#include "Logger.h"
#include "reactor/stop_signal.hh"
#include "service.hh"
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <cstdlib>
#include <exception>
#include <string>

namespace
{
seastar::future<> run(seastar::app_template& app)
{
    const char* configured = std::getenv("IMM_SERVER_LOG_LEVEL");
    const auto level = configured ? std::string(configured) : app.configuration()["log_level"].as<std::string>();
    imm::Logger::instance().setLevel(spdlog::level::from_str(level));
    const auto config = imm::state_service::load_options(imm::configuration::server());
    imm::io::initialize_grpc();
    const imm::state_service::scheduling_groups groups{co_await seastar::create_scheduling_group("protocol", 100),
                                                       co_await seastar::create_scheduling_group("transaction", 100),
                                                       co_await seastar::create_scheduling_group("io", 200)};
    seastar::sharded<imm::state_service::service> services;
    imm::io::stop_signal stop;
    bool constructed{};
    std::exception_ptr failure;
    try {
        co_await services.start(config, groups);
        constructed = true;
        co_await services.invoke_on_all([groups](imm::state_service::service& local) {
            return seastar::with_scheduling_group(groups.io, [&local] { return local.start(); });
        });
        LOG_INFO(imm::logger, "[state/startup] ready port={} shards={}", config.listener.port,
                 seastar::this_smp_shard_count());
        co_await stop.wait();
    } catch (...) {
        failure = std::current_exception();
    }
    if (constructed) {
        co_await services.invoke_on_all([groups](imm::state_service::service& local) {
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
    imm::Logger::instance().initialize(argv[0], imm::Logger::Service::State);
    const char* path = std::getenv("IMM_CONFIG");
    if (!imm::configuration::load(path ? path : "loopback/single.yaml")) {
        LOG_ERROR(imm::logger, "State config load failed");
        return EXIT_FAILURE;
    }
    seastar::app_template::seastar_options options;
    options.smp_opts.smp.set_default_value(imm::configuration::server()["self"]["shards"].as<unsigned>(1));
    seastar::app_template app(std::move(options));
    app.add_options()("log_level", boost::program_options::value<std::string>()->default_value("info"),
                      "imm log level");
    const int status = app.run(argc, argv, [&app] { return run(app); });
    imm::io::shutdown_grpc();
    return status;
}
