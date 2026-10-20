#include "reactor/grpc_engine.hh"
#include <grpcpp/grpcpp.h>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/task.hh>
#include <seastar/core/timer.hh>
#include "reactor/dns_resolver.hh"
#include <algorithm>
#include <cstring>
#include <map>
#include <cstdlib>
#include <stdexcept>
#include <pthread.h>
#include <signal.h>
#include <system_error>
#include <arpa/inet.h>
#include <charconv>
#include "src/core/lib/event_engine/extensions/blocking_dns.h"

namespace imm::io
{
namespace
{
namespace ee = grpc_event_engine::experimental;

struct observations {
    unsigned connects{};
    unsigned reads{};
    unsigned writes{};
    unsigned tasks{};
    unsigned timers{};
    unsigned endpoints{};
    unsigned active_io{};
    unsigned active_tasks{};
    unsigned dns_lookups{};
    unsigned resolvers{};
    unsigned listeners{};
};

// 每个 reactor 线程独立统计。对象析构不等于已排队任务或挂起 I/O 完成，
// 因而停止时同时检查活动计数及 engine 中的连接/定时器，避免过早退出 reactor。
static thread_local observations observed;

static void record(const char*)
{
    if (!seastar::engine_is_ready()) {
        std::terminate();
    }
}

static ee::EventEngine::ResolvedAddress resolved(const seastar::socket_address& address)
{
    return {&address.u.sa, address.addr_length};
}

class reactor_endpoint final : public ee::EventEngine::Endpoint
{
    struct state {
        seastar::pollable_fd fd;
        ee::MemoryAllocator allocator;
        bool closing{};
        bool reading{};
        bool writing{};
        state(seastar::pollable_fd descriptor, ee::MemoryAllocator memory)
            : fd(std::move(descriptor)), allocator(std::move(memory))
        {
        }
    };

  private:
    std::shared_ptr<state> _state;
    ee::EventEngine::ResolvedAddress _peer;
    ee::EventEngine::ResolvedAddress _local;

    static seastar::future<> read(std::shared_ptr<state> connection, absl::AnyInvocable<void(absl::Status)> callback,
                                  ee::SliceBuffer* buffer)
    {
        absl::Status status;
        try {
            auto allocation = connection->allocator.MakeSlice(ee::MemoryRequest(8192));
            ee::Slice slice(allocation);
            const auto count = co_await connection->fd.read_some(GRPC_SLICE_START_PTR(allocation), slice.size());
            if (count == 0) {
                status = absl::UnavailableError("peer closed");
            } else {
                buffer->Append(slice.TakeSubSlice(0, count));
            }
        } catch (const std::exception& error) {
            status = absl::UnavailableError(error.what());
        }
        record("Read.callback");
        connection->reading = false;
        --observed.active_io;
        callback(connection->closing ? absl::CancelledError("endpoint closed") : status);
    }

    static seastar::future<> write(std::shared_ptr<state> connection, absl::AnyInvocable<void(absl::Status)> callback,
                                   ee::SliceBuffer* buffer)
    {
        absl::Status status;
        try {
            while (buffer->Count() != 0) {
                auto slice = buffer->TakeFirst();
                iovec segment{const_cast<uint8_t*>(slice.data()), slice.size()};
                co_await connection->fd.send_all(std::span<iovec>(&segment, 1));
            }
        } catch (const std::exception& error) {
            status = absl::UnavailableError(error.what());
        }
        record("Write.callback");
        connection->writing = false;
        --observed.active_io;
        callback(connection->closing ? absl::CancelledError("endpoint closed") : status);
    }

  public:
    reactor_endpoint(seastar::pollable_fd descriptor, ee::MemoryAllocator allocator)
        : _state(std::make_shared<state>(std::move(descriptor), std::move(allocator))),
          _peer(resolved(_state->fd.get_file_desc().get_remote_address())),
          _local(resolved(_state->fd.get_file_desc().get_address()))
    {
        ++observed.endpoints;
    }

    ~reactor_endpoint() override
    {
        record("Endpoint.destroy");
        _state->closing = true;
        // endpoint 析构先唤醒在途读写；协程按值持有共享 state，
        // 直到取消回调完成才释放 fd，不继续借用已析构的 endpoint。
        _state->fd.shutdown(SHUT_RDWR);
        --observed.endpoints;
    }

    bool Read(absl::AnyInvocable<void(absl::Status)> callback, ee::SliceBuffer* buffer, ReadArgs) override
    {
        record("Read");
        ++observed.reads;
        ++observed.active_io;
        if (std::exchange(_state->reading, true)) {
            std::abort();
        }
        seastar::schedule(seastar::make_task([connection = _state, callback = std::move(callback), buffer]() mutable {
            (void)read(std::move(connection), std::move(callback), buffer);
        }));
        return false;
    }

    bool Write(absl::AnyInvocable<void(absl::Status)> callback, ee::SliceBuffer* buffer, WriteArgs) override
    {
        record("Write");
        ++observed.writes;
        ++observed.active_io;
        if (std::exchange(_state->writing, true)) {
            std::abort();
        }
        seastar::schedule(seastar::make_task([connection = _state, callback = std::move(callback), buffer]() mutable {
            (void)write(std::move(connection), std::move(callback), buffer);
        }));
        return false;
    }

    const ee::EventEngine::ResolvedAddress& GetPeerAddress() const override { return _peer; }
    const ee::EventEngine::ResolvedAddress& GetLocalAddress() const override { return _local; }
    std::shared_ptr<TelemetryInfo> GetTelemetryInfo() const override { return nullptr; }
};

class reactor_listener final : public ee::EventEngine::Listener
{
    struct state {
        AcceptCallback accept;
        absl::AnyInvocable<void(absl::Status)> shutdown;
        std::unique_ptr<ee::MemoryAllocatorFactory> allocator;
        std::vector<std::unique_ptr<seastar::pollable_fd>> sockets;
        unsigned active{};
        bool closing{};
        bool started{};
        absl::Status status;
        void complete()
        {
            if (closing && active == 0 && shutdown) {
                --observed.listeners;
                auto callback = std::move(shutdown);
                callback(status);
            }
        }
    };
    std::shared_ptr<state> _state;

    static seastar::future<> accept(std::shared_ptr<state> listener, seastar::pollable_fd& socket)
    {
        try {
            while (!listener->closing) {
                // 关闭监听会使等待中的 accept 失败；先保留并检查完成 future，
                // 只有成功时才取出拥有 socket 的结果，关闭异常路径不构造该结果对象。
                auto incoming = co_await seastar::coroutine::as_future(socket.accept());
                if (incoming.failed() && listener->closing) {
                    incoming.ignore_ready_future();
                    break;
                }
                auto accepted = incoming.get();
                if (listener->closing) {
                    break;
                }
                auto allocator = listener->allocator->CreateMemoryAllocator("seastar.grpc.accept");
                auto endpoint =
                    std::make_unique<reactor_endpoint>(std::move(std::get<0>(accepted)), std::move(allocator));
                listener->accept(std::move(endpoint),
                                 listener->allocator->CreateMemoryAllocator("seastar.grpc.callback"));
            }
        } catch (const std::exception& error) {
            if (!listener->closing) {
                listener->status = absl::UnavailableError(error.what());
            }
        }
        --listener->active;
        --observed.active_io;
        listener->complete();
    }

  public:
    reactor_listener(AcceptCallback on_accept, absl::AnyInvocable<void(absl::Status)> on_shutdown,
                     std::unique_ptr<ee::MemoryAllocatorFactory> allocator)
        : _state(std::make_shared<state>())
    {
        _state->accept = std::move(on_accept);
        _state->shutdown = std::move(on_shutdown);
        _state->allocator = std::move(allocator);
        ++observed.listeners;
    }
    ~reactor_listener() override
    {
        record("Listener.destroy");
        _state->closing = true;
        for (auto& socket : _state->sockets) {
            socket->shutdown(SHUT_RDWR);
        }
        // accept 协程持有 state；shutdown 回调须等所有 accept 和其同步回调退出。
        auto listener = std::move(_state);
        listener->complete();
    }
    absl::StatusOr<int> Bind(const ee::EventEngine::ResolvedAddress& source) override
    {
        record("Listener.Bind");
        if (_state->started || source.size() > sizeof(seastar::socket_address::u)) {
            return absl::InvalidArgumentError("invalid listener bind");
        }
        try {
            seastar::socket_address address;
            std::memcpy(&address.u, source.address(), source.size());
            address.addr_length = source.size();
            auto fd = seastar::engine().make_pollable_fd(address, IPPROTO_TCP);
            auto& raw = fd.get_file_desc();
            raw.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1);
            raw.bind(address.u.sa, address.addr_length);
            raw.listen(1024);
            auto bound = raw.get_address();
            const int port =
                bound.u.sa.sa_family == AF_INET ? ntohs(bound.u.in.sin_port) : ntohs(bound.u.in6.sin6_port);
            _state->sockets.push_back(std::make_unique<seastar::pollable_fd>(std::move(fd)));
            return port;
        } catch (const std::exception& error) {
            return absl::UnavailableError(error.what());
        }
    }
    absl::Status Start() override
    {
        record("Listener.Start");
        if (_state->started) {
            return absl::FailedPreconditionError("listener already started");
        }
        _state->started = true;
        for (auto& socket : _state->sockets) {
            ++_state->active;
            ++observed.active_io;
            seastar::schedule(
                seastar::make_task([listener = _state, socket = socket.get()] { (void)accept(listener, *socket); }));
        }
        return absl::OkStatus();
    }
};

// 每 shard 的 resolver 在首次初始化时读取系统配置，所有 Channel 复用它。
// 重连只新建协议状态，不能在运行期重复打开 hosts/resolver 配置文件。
thread_local std::shared_ptr<dns_resolver> local_dns;
std::shared_ptr<dns_resolver> shard_dns()
{
    if (!local_dns) {
        local_dns = std::make_shared<dns_resolver>();
    }
    return local_dns;
}

class reactor_dns final : public ee::EventEngine::DNSResolver, public ee::ResolverSupportsBlockingLookups
{
  public:
    void* QueryExtension(absl::string_view name) override
    {
        if (name == ee::ResolverSupportsBlockingLookups::EndpointExtensionName()) {
            return static_cast<ee::ResolverSupportsBlockingLookups*>(this);
        }
        return nullptr;
    }
    absl::StatusOr<std::vector<ee::EventEngine::ResolvedAddress>>
    LookupHostnameBlocking(absl::string_view input, absl::string_view default_port) override
    {
        // gRPC 建服务端监听时同步等待解析，即使输入是数字 IP。此扩展只做数字
        // 地址转换，不执行 DNS/文件/网络访问；域名须由调用方先异步解析后再绑定。
        std::string name(input);
        auto separator = name.rfind(':');
        std::string host = separator == std::string::npos ? name : name.substr(0, separator);
        std::string port_text = separator == std::string::npos ? std::string(default_port) : name.substr(separator + 1);
        if (host.starts_with('[') && host.ends_with(']')) {
            host = host.substr(1, host.size() - 2);
        }
        unsigned port{};
        const auto parsed = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != port_text.data() + port_text.size() || port > 65535) {
            return absl::InvalidArgumentError("listener requires numeric port");
        }
        sockaddr_in ipv4{};
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &ipv4.sin_addr) == 1) {
            return std::vector<ee::EventEngine::ResolvedAddress>{{reinterpret_cast<sockaddr*>(&ipv4), sizeof(ipv4)}};
        }
        sockaddr_in6 ipv6{};
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(port);
        if (::inet_pton(AF_INET6, host.c_str(), &ipv6.sin6_addr) == 1) {
            return std::vector<ee::EventEngine::ResolvedAddress>{{reinterpret_cast<sockaddr*>(&ipv6), sizeof(ipv6)}};
        }
        return absl::InvalidArgumentError("resolve listener hostname asynchronously before binding");
    }
    absl::StatusOr<std::vector<SRVRecord>> LookupSRVBlocking(absl::string_view) override
    {
        return absl::UnimplementedError("blocking SRV lookup is forbidden");
    }
    absl::StatusOr<std::vector<std::string>> LookupTXTBlocking(absl::string_view) override
    {
        return absl::UnimplementedError("blocking TXT lookup is forbidden");
    }
    struct state {
        std::shared_ptr<dns_resolver> resolver{shard_dns()};
        bool closing{};
    };
    std::shared_ptr<state> _state;

    static seastar::future<> lookup(std::shared_ptr<state> owner, LookupHostnameCallback callback, std::string name,
                                    std::string default_port)
    {
        absl::StatusOr<std::vector<ee::EventEngine::ResolvedAddress>> result;
        try {
            const auto separator = name.rfind(':');
            const auto port = std::stoi(separator == std::string::npos ? default_port : name.substr(separator + 1));
            const auto hostname = separator == std::string::npos ? name : name.substr(0, separator);
            auto host = co_await owner->resolver->resolve_all(hostname);
            std::vector<ee::EventEngine::ResolvedAddress> addresses;
            for (const auto& address : host) {
                addresses.push_back(resolved(seastar::socket_address(address, static_cast<uint16_t>(port))));
            }
            result = std::move(addresses);
        } catch (const std::exception& error) {
            result = absl::NotFoundError(error.what());
        }
        record("DNS.callback");
        --observed.active_io;
        callback(owner->closing ? absl::StatusOr<std::vector<ee::EventEngine::ResolvedAddress>>(
                                      absl::CancelledError("resolver closed"))
                                : std::move(result));
    }

  public:
    explicit reactor_dns(std::shared_ptr<state> owner) : _state(std::move(owner)) { ++observed.resolvers; }
    ~reactor_dns() override
    {
        record("DNS.destroy");
        --observed.resolvers;
    }
    void LookupHostname(LookupHostnameCallback callback, absl::string_view name,
                        absl::string_view default_port) override
    {
        record("DNS.LookupHostname");
        ++observed.dns_lookups;
        ++observed.active_io;
        seastar::schedule(seastar::make_task([owner = _state, callback = std::move(callback), name = std::string(name),
                                              port = std::string(default_port)]() mutable {
            (void)lookup(std::move(owner), std::move(callback), std::move(name), std::move(port));
        }));
    }
    void LookupSRV(LookupSRVCallback callback, absl::string_view) override
    {
        record("DNS.LookupSRV");
        ++observed.active_tasks;
        seastar::schedule(seastar::make_task([callback = std::move(callback)]() mutable {
            callback(absl::UnimplementedError("SRV is not used by the Gate client"));
            --observed.active_tasks;
        }));
    }
    void LookupTXT(LookupTXTCallback callback, absl::string_view) override
    {
        record("DNS.LookupTXT");
        ++observed.active_tasks;
        seastar::schedule(seastar::make_task([callback = std::move(callback)]() mutable {
            callback(absl::UnimplementedError("TXT is not used by the Gate client"));
            --observed.active_tasks;
        }));
    }
};

class reactor_engine final : public ee::EventEngine
{
    unsigned _owner;
    std::shared_ptr<reactor_dns::state> _dns;

    void require_owner(const char* event) const
    {
        record(event);
        if (seastar::this_shard_id() != _owner) {
            std::terminate();
        }
    }

    struct pending_timer {
        absl::AnyInvocable<void()> callback;
        seastar::timer<> timer;
    };
    struct pending_connection {
        seastar::pollable_fd fd;
        OnConnectCallback callback;
        ee::MemoryAllocator allocator;
        seastar::timer<> timeout;
        bool cancelled{};
        bool expired{};
        pending_connection(seastar::pollable_fd descriptor, OnConnectCallback completion, ee::MemoryAllocator memory)
            : fd(std::move(descriptor)), callback(std::move(completion)), allocator(std::move(memory))
        {
        }
    };
    std::map<intptr_t, std::unique_ptr<pending_timer>> _timers;
    std::map<intptr_t, std::shared_ptr<pending_connection>> _connections;
    intptr_t _next_id{};

    static seastar::future<> connect(std::shared_ptr<reactor_engine> engine, intptr_t id,
                                     std::shared_ptr<pending_connection> pending, seastar::socket_address address)
    {
        absl::Status status;
        try {
            co_await pending->fd.connect(address);
        } catch (const std::exception& error) {
            status = absl::UnavailableError(error.what());
        }
        engine->require_owner("Connect.callback");
        --observed.active_io;
        pending->timeout.cancel();
        engine->_connections.erase(id);
        if (pending->cancelled) {
            co_return;
        }
        auto callback = std::move(pending->callback);
        if (pending->expired) {
            callback(absl::DeadlineExceededError("connect timeout"));
        } else if (!status.ok()) {
            callback(status);
        } else {
            callback(
                std::unique_ptr<Endpoint>(new reactor_endpoint(std::move(pending->fd), std::move(pending->allocator))));
        }
    }

  public:
    explicit reactor_engine(unsigned owner) : _owner(owner), _dns(std::make_shared<reactor_dns::state>()) {}
    seastar::future<> close_dns()
    {
        _dns->closing = true;
        return seastar::make_ready_future<>();
    }

    bool IsWorkerThread() override { return seastar::engine_is_ready(); }

    void Run(Closure* closure) override
    {
        Run([closure] { closure->Run(); });
    }
    void Run(absl::AnyInvocable<void()> callback) override
    {
        // 即使 Channel 指定了 engine，gRPC 的 callback CQ 停止仍可能使用全局默认 engine。
        // Run 不访问 engine 局部状态，因此把任务排入调用它的 reactor，保留该 Channel 的 shard；
        // 带连接、DNS、定时器状态的接口则通过 require_owner 检查归属。
        record("Run");
        ++observed.tasks;
        ++observed.active_tasks;
        seastar::schedule(seastar::make_task([callback = std::move(callback)]() mutable {
            record("Run.callback");
            callback();
            --observed.active_tasks;
        }));
    }

    TaskHandle RunAfter(Duration delay, Closure* closure) override
    {
        return RunAfter(delay, [closure] { closure->Run(); });
    }
    TaskHandle RunAfter(Duration delay, absl::AnyInvocable<void()> callback) override
    {
        require_owner("RunAfter");
        ++observed.timers;
        auto id = ++_next_id;
        auto pending = std::make_unique<pending_timer>();
        pending->callback = std::move(callback);
        pending->timer.set_callback([this, id] {
            require_owner("RunAfter.callback");
            auto node = _timers.extract(id);
            auto callback = std::move(node.mapped()->callback);
            callback();
        });
        pending->timer.arm(seastar::timer<>::clock::now() +
                           std::chrono::duration_cast<seastar::timer<>::duration>(std::max(delay, Duration::zero())));
        _timers.emplace(id, std::move(pending));
        return {{id, 0}};
    }
    bool Cancel(TaskHandle handle) override
    {
        require_owner("Cancel");
        return _timers.erase(handle.keys[0]) != 0;
    }

    ConnectionHandle Connect(OnConnectCallback callback, const ResolvedAddress& remote, const ee::EndpointConfig&,
                             ee::MemoryAllocator allocator, Duration timeout) override
    {
        require_owner("Connect");
        ++observed.connects;
        ++observed.active_io;
        seastar::socket_address address;
        address.addr_length = remote.size();
        std::memcpy(&address.u.sa, remote.address(), remote.size());
        auto id = ++_next_id;
        auto pending = std::make_shared<pending_connection>(seastar::engine().make_pollable_fd(address, IPPROTO_TCP),
                                                            std::move(callback), std::move(allocator));
        pending->timeout.set_callback([pending = std::weak_ptr(pending)] {
            if (auto connection = pending.lock()) {
                connection->expired = true;
                connection->fd.shutdown(SHUT_RDWR);
            }
        });
        pending->timeout.arm(seastar::timer<>::clock::now() +
                             std::chrono::duration_cast<seastar::timer<>::duration>(timeout));
        _connections.emplace(id, pending);
        Run([engine = std::static_pointer_cast<reactor_engine>(shared_from_this()), id, pending, address] {
            (void)connect(engine, id, pending, address);
        });
        return {{id, 0}};
    }
    bool CancelConnect(ConnectionHandle handle) override
    {
        require_owner("CancelConnect");
        auto found = _connections.find(handle.keys[0]);
        if (found == _connections.end()) {
            return false;
        }
        auto pending = found->second;
        _connections.erase(found);
        // 从表中移除只表示取消已被接收；shutdown 后连接协程仍须恢复并退出。
        // 该协程持有 pending，并在结束时归还 active_io，因此不能只凭表为空判断停止完成。
        pending->cancelled = true;
        pending->callback = nullptr;
        pending->timeout.cancel();
        pending->fd.shutdown(SHUT_RDWR);
        return true;
    }

    absl::StatusOr<std::unique_ptr<DNSResolver>> GetDNSResolver(const DNSResolver::ResolverOptions& options) override
    {
        require_owner("GetDNSResolver");
        if (!options.dns_server.empty()) {
            return absl::UnimplementedError("custom gRPC DNS server option");
        }
        return std::unique_ptr<DNSResolver>(new reactor_dns(_dns));
    }
    absl::StatusOr<std::unique_ptr<Listener>>
    CreateListener(Listener::AcceptCallback on_accept, absl::AnyInvocable<void(absl::Status)> on_shutdown,
                   const ee::EndpointConfig&, std::unique_ptr<ee::MemoryAllocatorFactory> allocator) override
    {
        require_owner("CreateListener");
        return std::unique_ptr<Listener>(
            new reactor_listener(std::move(on_accept), std::move(on_shutdown), std::move(allocator)));
    }
    size_t pending_count() const { return _timers.size() + _connections.size(); }
};

std::shared_ptr<reactor_engine> default_engine;
} // namespace
void initialize_grpc()
{
    // 即使网络已接入 EventEngine，gRPC 仍会创建全局 timer/executor 线程。
    // Seastar 的信号处理依赖线程本地 reactor；初始化前屏蔽信号，让新线程继承掩码，
    // 作用域退出时恢复当前 reactor 的掩码，防止停止信号落到非 reactor 线程。
    sigset_t blocked, previous;
    sigfillset(&blocked);
    const int status = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (status != 0) {
        throw std::system_error(status, std::system_category(), "block gRPC initialization signals");
    }
    auto restore = seastar::defer([&previous]() noexcept { pthread_sigmask(SIG_SETMASK, &previous, nullptr); });
    setenv(
        "GRPC_EXPERIMENTS",
        "event_engine_client,event_engine_listener,event_engine_callback_cq,use_call_event_engine_in_completion_queue",
        1);
    default_engine = std::make_shared<reactor_engine>(seastar::this_shard_id());
    ee::SetDefaultEventEngine(default_engine);
    grpc_init();
}
seastar::future<> drain_local_grpc()
{
    // 所有本地 Channel 都已释放，才可取消共享 DNS 并等待遗留回调。
    if (local_dns) {
        co_await local_dns->close();
        local_dns.reset();
    }
    while (observed.active_io || observed.active_tasks || observed.endpoints || observed.resolvers ||
           observed.listeners) {
        co_await seastar::sleep(std::chrono::milliseconds(1));
    }
}
seastar::future<> drain_grpc()
{
    co_await close_grpc_engine(default_engine);
    co_await drain_local_grpc();
}
void shutdown_grpc()
{
    if (!default_engine) {
        return;
    }
    // 只在 app.run() 返回后执行线程 join；此前必须已在 reactor 内 drain 网络和回调，
    // 否则这里阻塞等待的任务已没有 reactor 可继续推进。
    grpc_shutdown_blocking();
    ee::SetDefaultEventEngine(nullptr);
    default_engine.reset();
}
std::shared_ptr<ee::EventEngine> make_grpc_engine()
{
    return std::make_shared<reactor_engine>(seastar::this_shard_id());
}
seastar::future<> close_grpc_engine(std::shared_ptr<ee::EventEngine> source)
{
    auto engine = std::static_pointer_cast<reactor_engine>(std::move(source));
    co_await engine->close_dns();
    while (engine->pending_count()) {
        co_await seastar::sleep(std::chrono::milliseconds(1));
    }
}
} // namespace imm::io
