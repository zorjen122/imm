#include "transport/connection.hh"
#include "api/protocol.hh"
#include "Logger.h"
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <seastar/core/coroutine.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <arpa/inet.h>
#include <cassert>
#include <cstring>

namespace imm::gateway_transport
{
namespace
{
constexpr size_t max_connection_writes = 1024;
constexpr size_t max_connection_bytes = 16 * 1024 * 1024;
} // namespace
connection::connection(seastar::connected_socket socket, server& owner)
    : _socket(std::move(socket)), _input(_socket.input()), _output(_socket.output(0)), _server(owner),
      _id(boost::uuids::to_string(boost::uuids::random_generator{}()))
{
    _socket.set_nodelay(true);
    _socket.set_keepalive(true);
}
connection::~connection() { assert(_closed && _writes.empty() && _reliable.empty()); }
seastar::future<> connection::run()
{
    try {
        while (!_closed) {
            const auto header = co_await _input.read_exactly(sizeof(uint32_t));
            if (header.empty()) {
                break;
            }
            if (header.size() != sizeof(uint32_t)) {
                break;
            }
            uint32_t encoded{};
            std::memcpy(&encoded, header.get(), sizeof(encoded));
            const auto bytes = ntohl(encoded);
            if (bytes > PROTOCOL_RECV_MSS) {
                break;
            }
            if (!bytes) {
                continue;
            }
            auto payload = co_await _input.read_exactly(bytes);
            if (payload.size() != bytes) {
                break;
            }
            co_await seastar::with_scheduling_group(
                _server.groups().protocol, [self = shared_from_this(), payload = std::move(payload)] {
                    auto envelope = gateway_protocol::decode(std::string_view(payload.get(), payload.size()));
                    return self->process(std::move(*envelope.mutable_request()));
                });
        }
    } catch (const std::exception& error) {
        if (!_closed) {
            LOG_DEBUG(logger, "[gateway/read] {}", error.what());
        }
    }
    close();
    co_await _requests.close();
    if (!_writer_closed) {
        co_await _writer.close();
        _writer_closed = true;
    }
    if (_uid > 0 && _lease.generation()) {
        try {
            co_await _server.transaction().remove(_uid, _lease);
        } catch (const std::exception& error) {
            LOG_DEBUG(logger, "[gateway/lease-cleanup] {}", error.what());
        }
    }
    try {
        co_await _input.close();
    } catch (...) {
    }
    try {
        co_await _output.close();
    } catch (...) {
    }
}
void connection::close()
{
    if (_closed) {
        return;
    }
    _closed = true;
    _server.transaction().cancel(_id);
    _writes.clear();
    _write_bytes = 0;
    _reliable.clear();
    _reliable_bytes = 0;
    _socket.shutdown_input();
    _socket.shutdown_output();
}
seastar::future<> connection::process(protocol::ClientRequest request)
{
    if (request.operation() != protocol::OP_LOGIN && _uid == 0) {
        error(request, ErrorCodes::AuthenticationRequired);
        co_return;
    }
    if (const int code = gateway_protocol::validate(request)) {
        error(request, code);
        co_return;
    }
    const auto& route_endpoint = gateway_protocol::route_for(request.operation());
    // 注册表的分类决定入口调度组；立即控制操作留在 protocol 组，其余进入 transaction。
    // 先取得 future，规避当前 GCC 对直接 co_await 成员函数指针调用表达式的编译器错误。
    if (route_endpoint.mode == gateway_protocol::processing::immediate) {
        auto result = (this->*route_endpoint.handle)(std::move(request));
        co_await std::move(result);
    } else {
        auto result =
            seastar::with_scheduling_group(_server.groups().transaction, [this, handle = route_endpoint.handle,
                                                                          request = std::move(request)]() mutable {
                return (this->*handle)(std::move(request));
            });
        co_await std::move(result);
    }
}
seastar::future<> connection::login(protocol::ClientRequest request)
{
    try {
        auto permit = _server.transaction().admit(request.ByteSizeLong());
        co_await _server.transaction().login(shared_from_this(), request);
    } catch (const io::overloaded&) {
        error(request, ErrorCodes::ResourceExhausted);
    } catch (const std::exception& failure) {
        LOG_DEBUG(logger, "[gateway/request] dependency failure: {}", failure.what());
        error(request, ErrorCodes::DependencyUnavailable);
    }
    co_return;
}
seastar::future<> connection::ping(protocol::ClientRequest request)
{
    protocol::Packet response;
    response.mutable_ping_response();
    SetTcpPacketStatus(response, ErrorCodes::Success);
    respond(request, std::move(response));
    co_return;
}
seastar::future<> connection::quit(protocol::ClientRequest request)
{
    protocol::Packet response;
    response.mutable_quit_response();
    SetTcpPacketStatus(response, ErrorCodes::Success);
    _close_after_write = true;
    respond(request, std::move(response));
    _writer_closed = true;
    co_await _writer.close();
    close();
    co_return;
}
seastar::future<> connection::acknowledge_request(protocol::ClientRequest request)
{
    acknowledge(request.packet().receipt().seq());
    if (request.packet().receipt().conversation_id() > 0 && request.packet().receipt().conversation_seq() > 0) {
        co_await forward(std::move(request));
    }
}
seastar::future<> connection::forward(protocol::ClientRequest request)
{
    if (request.operation() != protocol::OP_ACKNOWLEDGE) {
        const auto now = std::chrono::steady_clock::now();
        if (now - _rate_started >= std::chrono::seconds(1)) {
            _commands = 0;
            _rate_started = now;
        }
        if (_server.config().command_rate_limit && ++_commands > _server.config().command_rate_limit) {
            error(request, ErrorCodes::ResourceExhausted);
            co_return;
        }
    }
    try {
        auto permit = _server.transaction().admit(request.ByteSizeLong());
        // 不等待远端结果，读循环立即继续接收 ACK/后续命令；任务由连接 gate 持有。
        auto self = shared_from_this();
        (void)seastar::with_gate(_requests, [self, request = std::move(request), permit = std::move(permit)]() mutable {
            return seastar::with_scheduling_group(
                self->_server.groups().transaction,
                [self, request = std::move(request), permit = std::move(permit)]() mutable {
                    return process_forward(self, std::move(request), std::move(permit));
                });
        }).handle_exception([self](std::exception_ptr) { self->close(); });
    } catch (const io::overloaded&) {
        error(request, ErrorCodes::ResourceExhausted);
    } catch (const std::exception& failure) {
        LOG_DEBUG(logger, "[gateway/request] dependency failure: {}", failure.what());
        error(request, ErrorCodes::DependencyUnavailable);
    }
}
seastar::future<> connection::process_forward(std::shared_ptr<connection> self, protocol::ClientRequest request,
                                              gateway_transaction::request_permit permit)
{
    try {
        co_await self->_server.transaction().forward(self, request);
    } catch (const io::overloaded&) {
        self->error(request, ErrorCodes::ResourceExhausted);
    } catch (const std::exception& failure) {
        LOG_DEBUG(logger, "[gateway/command] dependency failure: {}", failure.what());
        self->error(request, ErrorCodes::DependencyUnavailable);
    }
}
seastar::future<bool> connection::refresh()
{
    if (_closed) {
        co_return false;
    }
    if (std::chrono::steady_clock::now() - _last_refresh < std::chrono::seconds(20)) {
        co_return true;
    }
    const bool current = co_await _server.transaction().refresh(_uid, _lease);
    if (!current) {
        close();
        co_return false;
    }
    _last_refresh = std::chrono::steady_clock::now();
    co_return !_closed;
}
bool connection::send(std::string envelope)
{
    if (_closed) {
        return false;
    }
    if (envelope.size() > PROTOCOL_SEND_MSS || _writes.size() >= max_connection_writes ||
        envelope.size() + sizeof(uint32_t) > max_connection_bytes - _write_bytes) {
        close();
        return false;
    }
    try {
        const auto bytes = envelope.size() + sizeof(uint32_t);
        auto units = _server.transaction().driver().writes.acquire(bytes);
        seastar::temporary_buffer<char> frame(bytes);
        const uint32_t length = htonl(envelope.size());
        std::memcpy(frame.get_write(), &length, sizeof(length));
        std::memcpy(frame.get_write() + sizeof(length), envelope.data(), envelope.size());
        _writes.push_back({std::move(frame), std::move(units)});
        _write_bytes += bytes;
        if (!_writing) {
            _writing = true;
            auto self = shared_from_this();
            (void)seastar::with_gate(_writer, [self] {
                return seastar::with_scheduling_group(self->_server.groups().io, [self] { return self->write_loop(); });
            }).handle_exception([self](std::exception_ptr) { self->close(); });
        }
        return true;
    } catch (const io::overloaded&) {
        close();
        return false;
    }
}
seastar::future<> connection::write_loop()
{
    while (!_writes.empty() && !_closed) {
        auto frame = std::move(_writes.front());
        _writes.pop_front();
        const auto bytes = frame.frame.size();
        co_await _output.write(std::move(frame.frame));
        co_await _output.flush();
        if (!_closed) {
            _write_bytes -= bytes;
        }
    }
    _writing = false;
    if (_close_after_write) {
        close();
    }
}
bool connection::send_reliable(std::string envelope, int64_t sequence)
{
    if (sequence <= 0) {
        return send(std::move(envelope));
    }
    auto previous = _reliable.find(sequence);
    if (previous != _reliable.end()) {
        // 同一序号也可能承载较新的投递状态（例如已读通知），按现有语义替换正文；
        // 保留条目额度，只调整字节额度，避免替换时临时占用第二个条目。
        const auto old_bytes = previous->second->envelope.size();
        if (envelope.size() > max_connection_bytes - (_reliable_bytes - old_bytes)) {
            close();
            return false;
        }
        try {
            _server.transaction().driver().reliable.resize(previous->second->units, envelope.size());
        } catch (const io::overloaded&) {
            close();
            return false;
        }
        if (!send(envelope) || _closed) {
            return false;
        }
        _reliable_bytes = _reliable_bytes - old_bytes + envelope.size();
        previous->second->timer.cancel();
        previous->second->envelope = std::move(envelope);
        previous->second->attempts = 1;
        previous->second->timer.arm(std::chrono::seconds(5));
        return true;
    }
    if (_closed || _reliable.size() >= max_connection_writes ||
        envelope.size() > max_connection_bytes - _reliable_bytes) {
        close();
        return false;
    }
    try {
        auto entry = std::make_unique<reliable_entry>(envelope,
                                                      _server.transaction().driver().reliable.acquire(envelope.size()));
        if (!send(std::move(envelope)) || _closed) {
            return false;
        }
        _reliable_bytes += entry->envelope.size();
        entry->timer.set_callback([self = weak_from_this(), sequence] {
            if (auto session = self.lock()) {
                session->retry(sequence);
            }
        });
        entry->timer.arm(std::chrono::seconds(5));
        _reliable.emplace(sequence, std::move(entry));
        return true;
    } catch (const io::overloaded&) {
        close();
        return false;
    }
}
void connection::retry(int64_t sequence)
{
    auto entry = _reliable.find(sequence);
    if (entry == _reliable.end() || _closed) {
        return;
    }
    if (entry->second->attempts >= 3) {
        acknowledge(sequence);
        return;
    }
    ++entry->second->attempts;
    const auto envelope = entry->second->envelope;
    if (send(envelope) && !_closed) {
        entry->second->timer.arm(std::chrono::seconds(5));
    }
}
void connection::acknowledge(int64_t sequence)
{
    auto found = _reliable.find(sequence);
    if (found != _reliable.end()) {
        _reliable_bytes -= found->second->envelope.size();
        _reliable.erase(found);
    }
}
void connection::respond(const protocol::ClientRequest& request, protocol::Packet packet)
{
    ClientEnvelope envelope;
    auto* response = envelope.mutable_response();
    response->set_request_id(request.request_id());
    response->set_operation(request.operation());
    *response->mutable_packet() = std::move(packet);
    send(envelope.SerializeAsString());
}
void connection::error(const protocol::ClientRequest& request, int code, std::string message)
{
    if (request.operation() != protocol::OP_ACKNOWLEDGE) {
        respond(request, MakeErrorPacket(code, message));
    }
}
} // namespace imm::gateway_transport
