#pragma once
#include <vector>
#include "io/options.hh"
#include <mysql.h>
#include <seastar/core/abort_source.hh>
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>
#include <optional>
#include <variant>

namespace wimi::gate_io
{
using mysql_parameter = std::variant<std::string, int64_t>;
struct mysql_result {
    std::vector<std::string> row;
    unsigned error{};
};

// One operation owns this connection through completion and close. A transaction
// must keep the same owner and connection for all statements and suspension points.
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
    void abort();
    seastar::future<> close();
};
} // namespace wimi::gate_io
