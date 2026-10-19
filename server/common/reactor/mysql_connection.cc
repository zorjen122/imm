#include "reactor/mysql_connection.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <array>
#include <cassert>
#include <fcntl.h>
#include <poll.h>
#include <system_error>

namespace wimi::io
{
mysql_connection::mysql_connection()
{
    if (!_mysql || ::mysql_options(_mysql, MYSQL_OPT_NONBLOCK, nullptr)) {
        throw std::runtime_error("MySQL async initialization failed");
    }
}
mysql_connection::~mysql_connection() { abort(); }
void mysql_connection::check(int status) const
{
    if (status != 0) {
        throw std::runtime_error(_statement ? mysql_stmt_error(_statement) : mysql_error(_mysql));
    }
}
void mysql_connection::abort()
{
    if (!_abort.abort_requested()) {
        _abort.request_abort();
    }
    _watch.reset();
    if (_mysql) {
        const int fd = mysql_get_socket(_mysql);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
        // Connector 允许关闭已挂起的异步操作。先 shutdown 传输，再释放客户端，
        // 使取消路径不会在发送 QUIT 时等待网络；wait() 恢复后会先识别 abort 状态。
        mysql_close(std::exchange(_mysql, nullptr));
        _statement = nullptr;
    }
}
seastar::future<int> mysql_connection::wait(int requested)
{
    if (requested & MYSQL_WAIT_EXCEPT) {
        throw std::runtime_error("unsupported MySQL exceptional socket wait");
    }
    // 只等待 Connector 的 _start/_cont 返回的事件；定时器采用库要求的超时，
    // 不在这里新增请求级 deadline，也不使用阻塞 poll 驱动网络。
    const auto duration = std::chrono::milliseconds(mysql_get_timeout_value_ms(_mysql));
    if (!(requested & (MYSQL_WAIT_READ | MYSQL_WAIT_WRITE))) {
        if (!(requested & MYSQL_WAIT_TIMEOUT)) {
            throw std::runtime_error("invalid MySQL wait mask");
        }
        co_await seastar::sleep_abortable(duration, _abort);
        co_return MYSQL_WAIT_TIMEOUT;
    }
    const int fd = mysql_get_socket(_mysql);
    const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) {
        throw std::system_error(errno, std::system_category(), "duplicate MySQL socket");
    }
    _watch.emplace(seastar::file_desc::from_fd(copy));
    bool expired{};
    seastar::timer<> timer([this, &expired] {
        expired = true;
        _watch.reset();
    });
    if (requested & MYSQL_WAIT_TIMEOUT) {
        timer.arm(std::chrono::steady_clock::now() + duration);
    }
    std::exception_ptr failure;
    try {
        if ((requested & (MYSQL_WAIT_READ | MYSQL_WAIT_WRITE)) == (MYSQL_WAIT_READ | MYSQL_WAIT_WRITE)) {
            co_await _watch->readable_or_writeable();
        } else if (requested & MYSQL_WAIT_READ) {
            co_await _watch->readable();
        } else {
            co_await _watch->writeable();
        }
    } catch (...) {
        failure = std::current_exception();
    }
    timer.cancel();
    _watch.reset();
    if (_abort.abort_requested()) {
        throw std::runtime_error("MySQL operation aborted");
    }
    if (expired) {
        co_return MYSQL_WAIT_TIMEOUT;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    // reactor 已完成就绪等待；timeout=0 的 poll 仅采样具体事件位，
    // 将其转换为 Connector 的续执行掩码，不在此等待网络。
    pollfd snapshot{fd, 0, 0};
    if (requested & MYSQL_WAIT_READ) {
        snapshot.events |= POLLIN;
    }
    if (requested & MYSQL_WAIT_WRITE) {
        snapshot.events |= POLLOUT;
    }
    if (::poll(&snapshot, 1, 0) < 0) {
        throw std::system_error(errno, std::system_category(), "MySQL readiness snapshot");
    }
    co_return (snapshot.revents & (POLLIN | POLLERR | POLLHUP) ? MYSQL_WAIT_READ : 0) |
        (snapshot.revents & (POLLOUT | POLLERR | POLLHUP) ? MYSQL_WAIT_WRITE : 0);
}
seastar::future<> mysql_connection::connect(std::string numeric_ip, const mysql_options& config)
{
    if (!config.ca_pem.empty()) {
        // 项目生成的 Connector TLS 适配从内存读取 CA，并用原始主机名做 SNI/证书校验；
        // TCP 连接使用已异步解析的 numeric_ip，避免客户端再同步解析域名或读取证书文件。
        ::mysql_optionsv(_mysql, MARIADB_OPT_USERDATA, "wimi_tls_peer_name",
                         const_cast<char*>(config.address.host.c_str()));
        my_bool enabled = 1;
        ::mysql_options(_mysql, MYSQL_OPT_SSL_ENFORCE, &enabled);
        ::mysql_options(_mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &enabled);
        mysql_ssl_set(_mysql, nullptr, nullptr, config.ca_pem.c_str(), nullptr, nullptr);
    }
    MYSQL* result{};
    int pending =
        mysql_real_connect_start(&result, _mysql, numeric_ip.c_str(), config.user.c_str(), config.password.c_str(),
                                 config.schema.c_str(), config.address.port, nullptr, 0);
    while (pending) {
        pending = mysql_real_connect_cont(&result, _mysql, co_await wait(pending));
    }
    if (!result) {
        throw std::runtime_error(mysql_error(_mysql));
    }
}
seastar::future<mysql_result> mysql_connection::query(std::string sql, std::vector<mysql_parameter> parameters)
{
    _statement = mysql_stmt_init(_mysql);
    if (!_statement) {
        throw std::runtime_error("MySQL statement allocation failed");
    }
    int status{};
    int pending = mysql_stmt_prepare_start(&status, _statement, sql.data(), sql.size());
    while (pending) {
        pending = mysql_stmt_prepare_cont(&status, _statement, co_await wait(pending));
    }
    check(status);
    // MYSQL_BIND 只借用参数内存，因此 parameters 必须按值留在本协程帧内，
    // 直到 prepare、execute、结果读取及 statement close 全部完成，不能绑定临时字符串。
    std::vector<MYSQL_BIND> arguments(parameters.size());
    for (size_t index = 0; index < parameters.size(); ++index) {
        auto& argument = arguments[index];
        if (auto* text = std::get_if<std::string>(&parameters[index])) {
            argument.buffer_type = MYSQL_TYPE_STRING;
            argument.buffer = text->data();
            argument.buffer_length = text->size();
        } else {
            argument.buffer_type = MYSQL_TYPE_LONGLONG;
            argument.buffer = &std::get<int64_t>(parameters[index]);
        }
    }
    check(mysql_stmt_bind_param(_statement, arguments.data()));
    pending = mysql_stmt_execute_start(&status, _statement);
    while (pending) {
        pending = mysql_stmt_execute_cont(&status, _statement, co_await wait(pending));
    }
    mysql_result result;
    if (status != 0) {
        result.error = mysql_stmt_errno(_statement);
        if (result.error != 1062) {
            check(status);
        }
    }
    const auto columns = mysql_stmt_field_count(_statement);
    if (columns && !result.error) {
        std::vector<std::array<char, 1024>> storage(columns);
        std::vector<unsigned long> lengths(columns);
        std::vector<my_bool> nulls(columns);
        std::vector<MYSQL_BIND> bindings(columns);
        for (unsigned index = 0; index < columns; ++index) {
            bindings[index].buffer_type = MYSQL_TYPE_STRING;
            bindings[index].buffer = storage[index].data();
            bindings[index].buffer_length = storage[index].size();
            bindings[index].length = &lengths[index];
            bindings[index].is_null = &nulls[index];
        }
        check(mysql_stmt_bind_result(_statement, bindings.data()));
        pending = mysql_stmt_store_result_start(&status, _statement);
        while (pending) {
            pending = mysql_stmt_store_result_cont(&status, _statement, co_await wait(pending));
        }
        check(status);
        pending = mysql_stmt_fetch_start(&status, _statement);
        while (pending) {
            pending = mysql_stmt_fetch_cont(&status, _statement, co_await wait(pending));
        }
        if (status != MYSQL_NO_DATA) {
            check(status);
            for (unsigned index = 0; index < columns; ++index) {
                if (nulls[index] || lengths[index] > storage[index].size()) {
                    throw std::runtime_error("invalid bounded query result");
                }
                result.row.emplace_back(storage[index].data(), lengths[index]);
            }
        }
    }
    my_bool closed{};
    auto* statement = _statement;
    pending = mysql_stmt_close_start(&closed, statement);
    while (pending) {
        pending = mysql_stmt_close_cont(&closed, statement, co_await wait(pending));
    }
    _statement = nullptr;
    check(closed);
    co_return result;
}
seastar::future<> mysql_connection::close()
{
    if (!_mysql) {
        co_return;
    }
    int pending = mysql_close_start(_mysql);
    while (pending) {
        pending = mysql_close_cont(_mysql, co_await wait(pending));
    }
    _mysql = nullptr;
}
} // namespace wimi::io
