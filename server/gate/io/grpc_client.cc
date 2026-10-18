#include "io/grpc_client.hh"
#include "state.pb.h"
#include <grpcpp/grpcpp.h>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/task.hh>
#include <seastar/core/timer.hh>
#include "io/dns_resolver.hh"
#include <algorithm>
#include <cstring>
#include <map>
#include <cstdlib>
#include <stdexcept>
#include <pthread.h>
#include <signal.h>
#include <system_error>

namespace wimi::gate_io
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

class reactor_dns final : public ee::EventEngine::DNSResolver
{
  public:
    struct state {
        dns_resolver resolver;
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
            auto host = co_await owner->resolver.resolve_all(hostname);
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
        return _dns->resolver.close();
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
    absl::StatusOr<std::unique_ptr<Listener>> CreateListener(Listener::AcceptCallback,
                                                             absl::AnyInvocable<void(absl::Status)>,
                                                             const ee::EndpointConfig&,
                                                             std::unique_ptr<ee::MemoryAllocatorFactory>) override
    {
        require_owner("CreateListener");
        return absl::UnimplementedError("client only");
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
seastar::future<> drain_grpc()
{
    co_await default_engine->close_dns();
    while (default_engine->pending_count() || observed.active_io || observed.active_tasks || observed.endpoints ||
           observed.resolvers) {
        co_await seastar::sleep(std::chrono::milliseconds(1));
    }
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
grpc_client::grpc_client(std::string target) : _engine(std::make_shared<reactor_engine>(seastar::this_shard_id()))
{
    grpc::ChannelArguments arguments;
    arguments.SetInt(GRPC_ARG_ENABLE_HTTP_PROXY, 0);
    arguments.SetInt(GRPC_ARG_SERVICE_CONFIG_DISABLE_RESOLUTION, 1);
    arguments.SetPointerWithVtable(GRPC_ARG_EVENT_ENGINE, &_engine, ee::grpc_event_engine_arg_vtable());
    _channel = grpc::CreateCustomChannel("dns:///" + target, grpc::InsecureChannelCredentials(), arguments);
    _stub = std::make_unique<grpc::GenericStub>(_channel);
}
seastar::future<gateway_node> grpc_client::pick_gateway(int64_t uid)
{
    state::ConnectUser source;
    source.set_id(uid);
    const std::string encoded = source.SerializeAsString();
    grpc::Slice slice(encoded);
    grpc::ByteBuffer request(&slice, 1), response;
    grpc::ClientContext context;
    _calls.insert(&context);
    auto remove = seastar::defer([this, &context]() noexcept { _calls.erase(&context); });
    seastar::promise<grpc::Status> completed;
    auto result = completed.get_future();
    const unsigned owner = seastar::this_shard_id();
    _stub->UnaryCall(&context, "/state.StateService/PickConnectionGateway", {}, &request, &response,
                     [&completed, owner](grpc::Status status) {
                         if (!seastar::engine_is_ready() || seastar::this_shard_id() != owner) {
                             std::terminate();
                         }
                         completed.set_value(std::move(status));
                     });
    const auto status = co_await std::move(result);
    if (!status.ok()) {
        throw std::runtime_error("State RPC failed: " + status.error_message());
    }
    std::vector<grpc::Slice> slices;
    if (!response.Dump(&slices).ok()) {
        throw std::runtime_error("invalid State RPC response");
    }
    std::string bytes;
    for (const grpc::Slice& part : slices) {
        bytes.append(reinterpret_cast<const char*>(part.begin()), part.size());
    }
    state::ConnectUserRsp message;
    if (!message.ParseFromString(bytes) || message.ip().empty() || message.port() <= 0 || message.port() > 65535) {
        throw std::runtime_error("State RPC returned no gateway");
    }
    co_return gateway_node{message.ip(), static_cast<uint16_t>(message.port()), message.node_id()};
}
void grpc_client::abort()
{
    for (grpc::ClientContext* context : _calls) {
        context->TryCancel();
    }
}
seastar::future<> grpc_client::close()
{
    if (!_calls.empty()) {
        throw std::logic_error("State RPC still running during close");
    }
    _stub.reset();
    _channel.reset();
    auto engine = std::static_pointer_cast<reactor_engine>(_engine);
    co_await engine->close_dns();
    while (observed.endpoints || observed.active_io || observed.active_tasks || observed.resolvers ||
           engine->pending_count()) {
        co_await seastar::sleep(std::chrono::milliseconds(1));
    }
    _engine.reset();
}
} // namespace wimi::gate_io
