#pragma once
#include "io/mysql_store.hh"
#include "io/operations.hh"
#include "io/options.hh"
#include "file.grpc.pb.h"
#include "reactor/dns_resolver.hh"
#include "reactor/grpc_engine.hh"
#include "reactor/redis_connection.hh"
#include <seastar/core/abort_source.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/semaphore.hh>
#include <set>

namespace imm::message_io
{
template <typename Operation>
concept mysql_operation = requires(mysql_store& store, Operation operation) { store.execute(std::move(operation)); };

class driver
{
    options _options;
    io::dns_resolver _dns;
    std::unique_ptr<io::mysql_connection> _mysql;
    std::unique_ptr<io::redis_connection> _redis;
    std::optional<io::endpoint> _redis_address;
    io::redis_connection* _sentinel{};
    seastar::semaphore _mysql_slot{1};
    seastar::semaphore _redis_slot{1};
    seastar::abort_source _abort;
    std::shared_ptr<grpc_event_engine::experimental::EventEngine> _file_engine;
    std::shared_ptr<grpc::Channel> _file_channel;
    std::unique_ptr<file::FileService::Stub> _file;
    std::set<grpc::ClientContext*> _file_calls;
    std::optional<seastar::future<>> _dns_closed;
    bool _mysql_initialized{};
    bool _stopping{};
    seastar::future<> connect_mysql();
    seastar::future<io::endpoint> discover_redis_master();
    seastar::future<io::redis_reply> redis_command(std::vector<std::string> arguments);
    seastar::future<gateway::SessionLease> session(int64_t uid, std::string device_id);
    void check_running() const;

  public:
    explicit driver(options config);
    ~driver();
    seastar::future<> start();
    // 先拒绝新任务并取消底层等待，再由所有者等待 operation_queue::stop()，最后 close()。
    void abort();
    seastar::future<> close();
    template <mysql_operation Operation> seastar::future<typename Operation::result_type> execute(Operation operation)
    {
        auto exclusive = co_await seastar::get_units(_mysql_slot, 1);
        check_running();
        if (_mysql && _mysql->aborted()) {
            _mysql.reset();
        }
        if (!_mysql) {
            co_await connect_mysql();
        }
        // 独占从连接准备持续到完整操作返回，覆盖事务内全部 SQL 和挂起点。
        // SQL 失败时丢弃连接；结果未知的写入绝不自动重放。
        try {
            mysql_store store(*_mysql);
            co_return co_await store.execute(std::move(operation));
        } catch (...) {
            _mysql->abort();
            throw;
        }
    }
    seastar::future<gateway::SessionLease> execute(get_session operation);
    seastar::future<std::vector<gateway::SessionLease>> execute(get_sessions operation);
    seastar::future<int64_t> execute(generate_id operation);
    seastar::future<file::UploadResponse> execute(upload_file operation);
    seastar::future<file::DownloadResponse> execute(download_file operation);
};
using operation_queue = io::operation_queue<driver, operations>;
} // namespace imm::message_io
