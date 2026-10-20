#pragma once
#include <vector>
#include "reactor/types.hh"
#include <mysql.h>
#include <seastar/core/abort_source.hh>
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>
#include <optional>
#include <variant>

namespace wimi::io
{
using mysql_parameter = std::variant<std::string, int64_t>;
struct mysql_result {
    std::vector<std::string> row;
    unsigned error{};
};
struct mysql_rows_result {
    std::vector<std::vector<std::optional<std::string>>> rows;
    uint64_t affected_rows{};
    uint64_t insert_id{};
    unsigned error{};
    std::string diagnostic;
};

// 调用方持有连接并保证同一时刻只有一个操作使用它；操作完成后可以继续复用。
// 完整事务在全部 SQL、挂起点及提交/回滚期间持续独占同一连接。
class mysql_connection
{
    MYSQL* _mysql{mysql_init(nullptr)};
    MYSQL_STMT* _statement{};
    std::optional<seastar::pollable_fd> _watch;
    seastar::abort_source _abort;
    seastar::future<int> wait(int requested);
    void check(int status) const;

  public:
    mysql_connection();
    ~mysql_connection();
    seastar::future<> connect(std::string numeric_ip, const mysql_options&);
    // Prepared statements bind owned parameters across suspension points. SELECT
    // callers must bound the result to one row; writes return execution status.
    // Duplicate-key errors are returned for business handling; other errors throw.
    seastar::future<mysql_result> query(std::string sql, std::vector<mysql_parameter> parameters);
    // 多行结果保留 NULL 与原始字节；prepare/execute/store/fetch/close 均通过
    // 非阻塞 API 驱动。异常后调用方须关闭连接，不在结果未知时自动重放事务。
    seastar::future<mysql_rows_result> query_rows(std::string sql, std::vector<mysql_parameter> parameters);
    // 不带结果集的事务边界/DDL；调用方只传可信 SQL，业务数据使用 query_rows 绑定。
    seastar::future<> command(std::string sql);
    void abort();
    bool aborted() const { return _abort.abort_requested(); }
    seastar::future<> close();
};
} // namespace wimi::io
