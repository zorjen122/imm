#pragma once
#include "io/fd_watch.hh"
#include <ares.h>
#include <seastar/net/inet_address.hh>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace wimi::gate_io
{
// c-ares is driven exclusively by reactor readiness and its next timeout; no
// c-ares event thread is enabled. Resolver configuration and hosts entries are
// loaded at construction, before HTTP admission. Live requests use the in-memory
// hosts snapshot or DNS packets, never synchronous hosts-file reads/stat calls.
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
} // namespace wimi::gate_io
