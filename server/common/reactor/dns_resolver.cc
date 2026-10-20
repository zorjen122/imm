#include "reactor/dns_resolver.hh"
#include "IoQueue.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

namespace wimi::io
{
namespace
{
std::string normalize(std::string host)
{
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
    return host;
}
struct lookup {
    dns_resolver* owner;
    seastar::promise<std::vector<seastar::net::inet_address>> completion;
};
} // namespace
dns_resolver::dns_resolver(std::string servers) : _timer([this] { process(ARES_SOCKET_BAD, ARES_SOCKET_BAD); })
{
    static std::once_flag initialization;
    std::call_once(initialization, [] {
        if (ares_library_init(ARES_LIB_INIT_ALL) != ARES_SUCCESS) {
            throw std::runtime_error("c-ares initialization failed");
        }
        std::atexit(ares_library_cleanup);
    });
    // hosts 和 resolver 配置只在启动时读取；请求期间查询 hosts 内存快照或发送 DNS 包。
    // 下方 lookups="b" 禁止 c-ares 文件查找，避免其缓存刷新触发同步 stat/read；
    // 修改系统 hosts/resolver 配置后需要重启服务。
    std::ifstream hosts("/etc/hosts");
    std::string line;
    while (std::getline(hosts, line)) {
        line.erase(line.find('#') == std::string::npos ? line.size() : line.find('#'));
        std::istringstream fields(line);
        std::string address, name;
        fields >> address;
        const auto ip = seastar::net::inet_address::parse_numerical(address);
        if (!ip) {
            continue;
        }
        while (fields >> name) {
            _hosts[normalize(std::move(name))].push_back(*ip);
        }
    }
    ares_options options{};
    char lookups[] = "b";
    options.lookups = lookups;
    options.sock_state_cb = socket_state;
    options.sock_state_cb_data = this;
    const int status = ares_init_options(&_channel, &options, ARES_OPT_LOOKUPS | ARES_OPT_SOCK_STATE_CB);
    if (status != ARES_SUCCESS) {
        throw std::runtime_error(ares_strerror(status));
    }
    if (!servers.empty() && ares_set_servers_ports_csv(_channel, servers.c_str()) != ARES_SUCCESS) {
        ares_destroy(std::exchange(_channel, nullptr));
        throw std::invalid_argument("invalid numeric DNS server list");
    }
}
dns_resolver::~dns_resolver() { assert(!_channel && _watches.empty() && _retired.empty() && !_pending); }
void dns_resolver::retire(int fd)
{
    const auto found = _watches.find(fd);
    if (found == _watches.end()) {
        return;
    }
    for (auto pending = _retired.begin(); pending != _retired.end();) {
        if (pending->available()) {
            pending->get();
            pending = _retired.erase(pending);
        } else {
            ++pending;
        }
    }
    auto watch = std::move(found->second);
    _watches.erase(found);
    _retired.push_back(watch->stop().then([watch] {}));
}
void dns_resolver::socket_state(void* state, ares_socket_t fd, int read, int write)
{
    auto& self = *static_cast<dns_resolver*>(state);
    if (!read && !write) {
        self.retire(fd);
        return;
    }
    if (!self._watches.contains(fd)) {
        self._watches.emplace(fd, fd_watch::create(
                                      fd, [&self, fd] { self.process(fd, ARES_SOCKET_BAD); },
                                      [&self, fd] { self.process(ARES_SOCKET_BAD, fd); }));
    }
    const auto watch = self._watches.at(fd);
    watch->set(0, read);
    watch->set(1, write);
}
void dns_resolver::update_timer()
{
    _timer.cancel();
    if (!_channel || !_pending) {
        return;
    }
    _timer.set_callback([this] { process(ARES_SOCKET_BAD, ARES_SOCKET_BAD); });
    timeval duration{};
    ares_timeout(_channel, nullptr, &duration);
    _timer.arm(std::chrono::steady_clock::now() + std::chrono::seconds(duration.tv_sec) +
               std::chrono::microseconds(duration.tv_usec));
}
void dns_resolver::process(ares_socket_t read, ares_socket_t write)
{
    if (!_channel) {
        return;
    }
    ares_process_fd(_channel, read, write);
    update_timer();
}
void dns_resolver::completed(void* state, int status, int, ares_addrinfo* raw)
{
    std::unique_ptr<lookup> pending(static_cast<lookup*>(state));
    std::unique_ptr<ares_addrinfo, decltype(&ares_freeaddrinfo)> result(raw, ares_freeaddrinfo);
    --pending->owner->_pending;
    if (!pending->owner->_channel) {
        pending->completion.set_exception(io::stopped());
        return;
    }
    if (status != ARES_SUCCESS) {
        pending->completion.set_exception(std::runtime_error(ares_strerror(status)));
        return;
    }
    std::vector<seastar::net::inet_address> addresses;
    for (auto* node = result->nodes; node; node = node->ai_next) {
        if (node->ai_family == AF_INET) {
            addresses.emplace_back(reinterpret_cast<sockaddr_in*>(node->ai_addr)->sin_addr);
        } else if (node->ai_family == AF_INET6) {
            addresses.emplace_back(reinterpret_cast<sockaddr_in6*>(node->ai_addr)->sin6_addr);
        }
    }
    if (addresses.empty()) {
        pending->completion.set_exception(std::runtime_error("DNS returned no addresses"));
        return;
    }
    pending->completion.set_value(std::move(addresses));
}
seastar::future<std::vector<seastar::net::inet_address>> dns_resolver::resolve_all(std::string host, int family)
{
    using addresses = std::vector<seastar::net::inet_address>;
    if (!_channel) {
        return seastar::make_exception_future<addresses>(io::stopped());
    }
    addresses local;
    if (auto ip = seastar::net::inet_address::parse_numerical(host)) {
        if (family == AF_UNSPEC || static_cast<int>(ip->in_family()) == family) {
            local.push_back(*ip);
        }
    } else if (auto found = _hosts.find(normalize(host)); found != _hosts.end()) {
        for (const auto& ip : found->second) {
            if (family == AF_UNSPEC || static_cast<int>(ip.in_family()) == family) {
                local.push_back(ip);
            }
        }
    }
    if (!local.empty()) {
        return seastar::make_ready_future<addresses>(std::move(local));
    }
    auto pending = std::make_unique<lookup>();
    pending->owner = this;
    auto future = pending->completion.get_future();
    ares_addrinfo_hints hints{};
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    // c-ares 默认排序会用 connect() 探测地址路由；这里不需要该排序，
    // 禁用它以免解析过程额外创建探测 socket。
    hints.ai_flags = ARES_AI_NOSORT;
    ++_pending;
    ares_getaddrinfo(_channel, host.c_str(), nullptr, &hints, completed, pending.release());
    update_timer();
    return future;
}
seastar::future<seastar::net::inet_address> dns_resolver::resolve_name(std::string host,
                                                                       seastar::net::inet_address::family family)
{
    auto addresses = co_await resolve_all(std::move(host), static_cast<int>(family));
    co_return addresses.front();
}
seastar::future<> dns_resolver::close()
{
    _timer.cancel();
    if (_channel) {
        // 先置空，再取消：ares_cancel 会同步触发尚未完成查询的回调，
        // 回调据此交付 stopped；随后还须等待所有 fd 监控任务退出才能销毁 resolver。
        auto* channel = std::exchange(_channel, nullptr);
        ares_cancel(channel);
        ares_destroy(channel);
    }
    while (!_watches.empty()) {
        retire(_watches.begin()->first);
    }
    co_await seastar::when_all_succeed(_retired.begin(), _retired.end());
    _retired.clear();
}
} // namespace wimi::io
