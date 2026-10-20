#pragma once
#include "reactor/fd_watch.hh"
#include <ares.h>
#include <seastar/net/inet_address.hh>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace imm::io
{
// c-ares 仅由 reactor 的 fd 就绪和库要求的定时器推进，不创建 DNS 事件线程。
// 构造时读取 resolver 配置和 hosts 快照，因此实例须在接收业务前创建并复用；
// 请求及重连阶段只查询内存或发送 DNS 包，不同步读取/stat 配置文件。
class dns_resolver
{
    ares_channel _channel{};
    seastar::timer<> _timer;
    std::map<int, std::shared_ptr<fd_watch>> _watches;
    std::vector<seastar::future<>> _retired;
    std::unordered_map<std::string, std::vector<seastar::net::inet_address>> _hosts;
    unsigned _pending{};
    static void socket_state(void*, ares_socket_t, int, int);
    static void completed(void*, int, int, ares_addrinfo*);
    void process(ares_socket_t read, ares_socket_t write);
    void update_timer();
    void retire(int fd);

  public:
    // An empty servers string uses the OS resolver configuration. An explicit
    // value is c-ares's numeric server[:port] list, also used by the DNS fixture.
    explicit dns_resolver(std::string servers = {});
    ~dns_resolver();
    dns_resolver(const dns_resolver&) = delete;
    dns_resolver& operator=(const dns_resolver&) = delete;
    seastar::future<std::vector<seastar::net::inet_address>> resolve_all(std::string host, int family = AF_UNSPEC);
    seastar::future<seastar::net::inet_address>
    resolve_name(std::string host,
                 seastar::net::inet_address::family family = seastar::net::inet_address::family::INET);
    // Cancels queries, destroys c-ares sockets, and joins every readiness task.
    // Must finish before destruction, including when no query has been made.
    seastar::future<> close();
};
} // namespace imm::io
