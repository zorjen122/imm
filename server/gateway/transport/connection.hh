#pragma once
#include "transport/server.hh"
#include "TcpMessageCodec.h"
#include <seastar/core/timer.hh>
#include <deque>
#include <map>
#include <chrono>

namespace imm::gateway_transport
{
class connection : public std::enable_shared_from_this<connection>
{
    struct write_entry {
        seastar::temporary_buffer<char> frame;
        gateway_io::capacity_units units;
    };
    struct reliable_entry {
        std::string envelope;
        gateway_io::capacity_units units;
        unsigned attempts{1};
        seastar::timer<> timer;
        reliable_entry(std::string body, gateway_io::capacity_units capacity)
            : envelope(std::move(body)), units(std::move(capacity))
        {
        }
    };
    seastar::connected_socket _socket;
    seastar::input_stream<char> _input;
    seastar::output_stream<char> _output;
    server& _server;
    std::string _id;
    int64_t _uid{};
    gateway::SessionLease _lease;
    seastar::gate _requests;
    seastar::gate _writer;
    std::deque<write_entry> _writes;
    std::map<int64_t, std::unique_ptr<reliable_entry>> _reliable;
    size_t _write_bytes{};
    size_t _reliable_bytes{};
    bool _writing{};
    bool _closed{};
    bool _close_after_write{};
    bool _writer_closed{};
    std::chrono::steady_clock::time_point _rate_started{std::chrono::steady_clock::now()};
    unsigned _commands{};
    std::chrono::steady_clock::time_point _last_refresh{};
    seastar::future<> process(protocol::ClientRequest request);
    static seastar::future<> process_forward(std::shared_ptr<connection> self, protocol::ClientRequest request,
                                             gateway_transaction::request_permit permit);
    seastar::future<> write_loop();
    void retry(int64_t sequence);
    void acknowledge(int64_t sequence);

  public:
    connection(seastar::connected_socket socket, server& owner);
    ~connection();
    seastar::future<> run();
    // 协议注册表绑定这些入口，新增操作不需要修改公共接收循环。
    seastar::future<> login(protocol::ClientRequest request);
    seastar::future<> ping(protocol::ClientRequest request);
    seastar::future<> quit(protocol::ClientRequest request);
    seastar::future<> acknowledge_request(protocol::ClientRequest request);
    seastar::future<> forward(protocol::ClientRequest request);
    void close();
    bool closed() const { return _closed; }
    const std::string& id() const { return _id; }
    int64_t uid() const { return _uid; }
    const gateway::SessionLease& lease() const { return _lease; }
    void bind(int64_t uid, gateway::SessionLease lease)
    {
        _uid = uid;
        _lease = std::move(lease);
        _last_refresh = std::chrono::steady_clock::now();
    }
    bool send(std::string envelope);
    bool send_reliable(std::string envelope, int64_t sequence);
    void respond(const protocol::ClientRequest& request, protocol::Packet packet);
    void error(const protocol::ClientRequest& request, int code, std::string message = {});
    seastar::future<> close_previous(gateway::SessionLease previous)
    {
        return _server.close_previous(std::move(previous));
    }
    seastar::future<bool> refresh();
};
} // namespace imm::gateway_transport
