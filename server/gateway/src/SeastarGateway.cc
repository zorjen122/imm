#include "SeastarGateway.h"

#include "ClientSession.h"
#include "Const.h"
#include "DbGlobal.h"
#include "Logger.h"
#include "MessageLinkManager.h"
#include "Mysql.h"
#include "Redis.h"
#include "SeastarExecutor.h"
#include "SessionManager.h"
#include "TcpMessageCodec.h"

#include <seastar/core/alien.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/timer.hh>
#include <seastar/util/defer.hh>

#include <arpa/inet.h>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <string_view>
#include <utility>

namespace wimi::connection
{
namespace
{

constexpr std::size_t kMaxQueuedWrites = 1024;
constexpr std::size_t kMaxQueuedWriteBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxReliableWrites = 1024;
constexpr std::size_t kMaxReliableWriteBytes = 16 * 1024 * 1024;

std::string NewUuid() { return boost::uuids::to_string(boost::uuids::random_generator{}()); }

std::string OperationName(protocol::Operation operation) { return protocol::Operation_Name(operation); }

struct SessionTarget {
    seastar::alien::instance* alien{};
    seastar::sharded<SeastarGatewayServer>* shards{};
    unsigned shard{};
    std::string connectionId;
};

class SessionProxy final : public ClientSession
{
  public:
    explicit SessionProxy(SessionTarget target) : target(std::move(target)) {}

    void Close() override
    {
        seastar::alien::run_on(*target.alien, target.shard, [target = target]() noexcept {
            try {
                target.shards->local().Close(target.connectionId);
            } catch (...) {
            }
        });
    }

    bool SendRaw(std::string envelope) override
    {
        try {
            return seastar::alien::submit_to(*target.alien, target.shard,
                                             [target = target, envelope = std::move(envelope)]() mutable {
                                                 return seastar::make_ready_future<bool>(target.shards->local().SendRaw(
                                                     target.connectionId, std::move(envelope)));
                                             })
                .get();
        } catch (...) {
            return false;
        }
    }

    bool SendReliable(std::string envelope, int64_t ackSeq) override
    {
        try {
            return seastar::alien::submit_to(
                       *target.alien, target.shard,
                       [target = target, envelope = std::move(envelope), ackSeq]() mutable {
                           return seastar::make_ready_future<bool>(
                               target.shards->local().SendReliable(target.connectionId, std::move(envelope), ackSeq));
                       })
                .get();
        } catch (...) {
            return false;
        }
    }

    const std::string& ConnectionId() const override { return target.connectionId; }

  private:
    SessionTarget target;
};

struct LoginResult {
    int error{ErrorCodes::InternalError};
    TcpPacket response;
    db::SessionLease lease;
    std::shared_ptr<ClientSession> proxy;
};

LoginResult HandleLogin(SessionManager& sessions, SessionTarget target, TcpPacket request, int64_t currentUid)
{
    LoginResult result;
    const auto& login = request.login_request();
    const int64_t uid = login.uid();
    if (uid <= 0 || login.device_id().empty() || login.device_id().size() > 36 || login.auth_token().empty() ||
        !db::RedisDao::GetInstance()->validateChatAuthToken(uid, login.auth_token())) {
        result.error = ErrorCodes::TokenInvalid;
        result.response = MakeErrorPacket(ErrorCodes::TokenInvalid, "invalid or expired connection token");
        return result;
    }
    if (currentUid > 0 && currentUid != uid) {
        result.error = ErrorCodes::UidInvalid;
        result.response = MakeErrorPacket(ErrorCodes::UidInvalid, "connection is already bound");
        return result;
    }

    db::UserInfo::Ptr info;
    if (login.init_profile()) {
        auto initialInfo =
            std::make_shared<db::UserInfo>(uid, login.name(), login.age(), login.sex(), login.head_image_url());
        if (db::MysqlDao::GetInstance()->insertUserInfo(initialInfo) == 0) {
            info = std::move(initialInfo);
        } else {
            info = db::MysqlDao::GetInstance()->getUserInfo(uid);
            if (!info) {
                result.error = ErrorCodes::MysqlFailed;
                result.response = MakeErrorPacket(ErrorCodes::MysqlFailed);
                return result;
            }
        }
    } else {
        info = db::MysqlDao::GetInstance()->getUserInfo(uid);
        if (!info) {
            result.error = ErrorCodes::NotFound;
            result.response = MakeErrorPacket(ErrorCodes::NotFound, "user profile not found");
            return result;
        }
    }

    const int deviceRegistration = db::MysqlDao::GetInstance()->registerDevice(uid, login.device_id());
    if (deviceRegistration <= 0) {
        result.error = deviceRegistration < 0 ? ErrorCodes::MysqlFailed : ErrorCodes::AuthenticationRequired;
        result.response = MakeErrorPacket(result.error, deviceRegistration < 0 ? "failed to register device"
                                                                               : "device is bound to another user");
        return result;
    }

    result.proxy = std::make_shared<SessionProxy>(std::move(target));
    result.lease = sessions.Bind(uid, login.device_id(), result.proxy);
    if (result.lease.empty()) {
        result.proxy.reset();
        result.error = ErrorCodes::DependencyUnavailable;
        result.response = MakeErrorPacket(ErrorCodes::DependencyUnavailable, "failed to publish session lease");
        return result;
    }

    result.error = ErrorCodes::Success;
    result.response.mutable_login_response()->set_uid(uid);
    SetTcpPacketStatus(result.response, ErrorCodes::Success);
    return result;
}

void PostCommandResult(const SessionTarget& target, const gateway::CommandResult& result, bool expectResponse,
                       int64_t actor, protocol::Operation operation, const std::string& requestId)
{
    std::string serialized;
    if (!result.SerializeToString(&serialized))
        return;
    seastar::alien::run_on(
        *target.alien, target.shard,
        [target, serialized = std::move(serialized), expectResponse, actor, operation, requestId]() mutable noexcept {
            try {
                target.shards->local().DeliverCommandResult(target.connectionId, std::move(serialized), expectResponse,
                                                            actor, static_cast<int>(operation), requestId);
            } catch (...) {
            }
        });
}

} // namespace

class SeastarGatewaySession : public std::enable_shared_from_this<SeastarGatewaySession>
{
    struct ReliableWrite {
        std::string envelope;
        unsigned int attempts{1};
        std::unique_ptr<seastar::timer<>> timer;
    };

  public:
    SeastarGatewaySession(seastar::connected_socket socket, SeastarGatewayServer& server, SeastarExecutor& executor,
                          SessionManager& sessions, MessageLinkManager& messageLinks, GatewayQueueBudget& queueBudget,
                          seastar::sharded<SeastarGatewayServer>* shards)
        : socket(std::move(socket)), input(this->socket.input()), output(this->socket.output(0)), server(server),
          executor(executor), sessions(sessions), messageLinks(messageLinks), queueBudget(queueBudget), shards(shards),
          connectionId(NewUuid())
    {
        this->socket.set_nodelay(true);
        this->socket.set_keepalive(true);
    }

    const std::string& ConnectionId() const { return connectionId; }

    seastar::future<> Run()
    {
        LOG_DEBUG(logger, "[gateway/session] 客户端连接已建立, connection_id: {}, shard: {}", connectionId,
                  seastar::this_shard_id());
        try {
            while (!closed) {
                auto header = co_await input.read_exactly(PROTOCOL_HEADER_TOTAL);
                if (header.empty())
                    break;
                if (header.size() != PROTOCOL_HEADER_TOTAL) {
                    LOG_WARN(logger, "[gateway/client-read] 协议头提前结束, connection_id: {}, bytes: {}", connectionId,
                             header.size());
                    break;
                }

                uint32_t wireSize{};
                std::memcpy(&wireSize, header.get(), sizeof(wireSize));
                const uint32_t bodySize = ntohl(wireSize);
                if (bodySize > PROTOCOL_RECV_MSS) {
                    LOG_WARN(logger, "[gateway/client-read] 拒绝无效数据帧, connection_id: {}, bytes: {}, limit: {}",
                             connectionId, bodySize, PROTOCOL_RECV_MSS);
                    break;
                }
                if (bodySize == 0) {
                    LOG_WARN(logger, "[gateway/client-read] 忽略空数据帧, connection_id: {}", connectionId);
                    continue;
                }

                auto payload = co_await input.read_exactly(bodySize);
                if (payload.size() != bodySize) {
                    LOG_WARN(logger,
                             "[gateway/client-read] 数据帧提前结束, connection_id: {}, expected: {}, actual: {}",
                             connectionId, bodySize, payload.size());
                    break;
                }
                co_await HandlePacket(payload.get(), payload.size());
            }
        } catch (const std::exception& error) {
            if (!closed)
                LOG_WARN(logger, "[gateway/session] 读取会话失败, connection_id: {}, error: {}", connectionId,
                         error.what());
        }

        Close();
        co_await writeGate.close();
        try {
            co_await input.close();
        } catch (...) {
        }
        try {
            co_await output.close();
        } catch (...) {
        }

        if (userId > 0 && !lease.empty() && proxy) {
            try {
                auto uid = userId;
                auto currentLease = lease;
                auto currentProxy = proxy;
                co_await executor.Submit(currentLease.deviceId.size() + currentLease.connectionId.size(),
                                         [this, uid, currentLease, currentProxy] {
                                             sessions.Remove(uid, currentLease.deviceId, currentProxy, currentLease);
                                             return true;
                                         });
            } catch (const ExecutorOverloaded&) {
                sessions.Discard(userId, lease.deviceId, proxy);
                LOG_WARN(logger,
                         "[gateway/session-remove] 清理队列已满, 本地会话已移除并等待 Redis 租约过期, uid: {}, "
                         "connection_id: {}",
                         userId, connectionId);
            } catch (const std::exception& error) {
                LOG_WARN(logger, "[gateway/session-remove] 会话清理失败, uid: {}, connection_id: {}, error: {}", userId,
                         connectionId, error.what());
            }
        }
        server.Erase(connectionId);
    }

    void Close()
    {
        if (closed)
            return;
        closed = true;
        LOG_DEBUG(logger,
                  "[gateway/session] 正在关闭客户端连接, uid: {}, connection_id: {}, queued_writes: {}, "
                  "queued_bytes: {}, pending_acks: {}",
                  userId, connectionId, writeQueue.size(), queuedWriteBytes, reliableWrites.size());
        for (auto& [_, write] : reliableWrites) {
            if (write.timer)
                write.timer->cancel();
            queueBudget.ReleaseReliable(write.envelope.size());
        }
        reliableWrites.clear();
        reliableWriteBytes = 0;
        std::size_t discardedWriteBytes = 0;
        for (const auto& frame : writeQueue) {
            queueBudget.ReleaseWrite(frame.size());
            discardedWriteBytes += frame.size();
        }
        writeQueue.clear();
        queuedWriteBytes -= discardedWriteBytes;
        socket.shutdown_input();
        socket.shutdown_output();
    }

    bool SendRaw(std::string envelope)
    {
        if (closed || envelope.size() > PROTOCOL_SEND_MSS)
            return false;

        seastar::temporary_buffer<char> frame(PROTOCOL_HEADER_TOTAL + envelope.size());
        const uint32_t wireSize = htonl(static_cast<uint32_t>(envelope.size()));
        std::memcpy(frame.get_write(), &wireSize, sizeof(wireSize));
        std::memcpy(frame.get_write() + PROTOCOL_HEADER_TOTAL, envelope.data(), envelope.size());
        return QueueFrame(std::move(frame));
    }

    bool SendReliable(std::string envelope, int64_t ackSeq)
    {
        if (ackSeq <= 0)
            return SendRaw(std::move(envelope));
        auto found = reliableWrites.find(ackSeq);
        const auto oldBytes = found == reliableWrites.end() ? 0 : found->second.envelope.size();
        if (envelope.size() > kMaxReliableWriteBytes - (reliableWriteBytes - oldBytes) ||
            (found == reliableWrites.end() && reliableWrites.size() >= kMaxReliableWrites)) {
            LOG_WARN(logger, "[gateway/delivery-queue] 可靠推送队列已满, connection_id: {}, entries: {}, bytes: {}",
                     connectionId, reliableWrites.size(), reliableWriteBytes);
            Close();
            return false;
        }
        if (!SendRaw(envelope))
            return false;

        const bool admitted = found == reliableWrites.end() ? queueBudget.TryAcquireReliable(envelope.size())
                                                            : queueBudget.TryResizeReliable(oldBytes, envelope.size());
        if (!admitted) {
            LOG_WARN(logger, "[gateway/delivery-queue] 进程可靠推送预算已满, connection_id: {}", connectionId);
            Close();
            return false;
        }

        if (found != reliableWrites.end()) {
            reliableWriteBytes -= oldBytes;
            if (found->second.timer)
                found->second.timer->cancel();
        }
        reliableWriteBytes += envelope.size();
        reliableWrites[ackSeq] = ReliableWrite{std::move(envelope), 1, {}};
        ArmReliableWrite(ackSeq);
        return true;
    }

    void HandleForwardResult(const gateway::CommandResult& result, bool expectResponse, int64_t actor,
                             protocol::Operation operation, const std::string& requestId)
    {
        if (result.error() == ErrorCodes::AuthenticationRequired) {
            LOG_WARN(logger,
                     "[gateway/command-result] Message 节点拒绝了过期连接, request_id: {}, uid: {}, "
                     "operation: {}",
                     requestId, actor, OperationName(operation));
            Close();
            return;
        }
        if (!expectResponse)
            return;

        TcpPacket packet;
        if (!ParseTcpPacket(result.packet(), packet)) {
            SendError(requestId, operation, ErrorCodes::ProtobufPacket, "invalid message response");
            return;
        }
        SendResponse(requestId, operation, std::move(packet));
    }

  private:
    SessionTarget Target() const
    {
        return SessionTarget{&seastar::engine().alien(), shards, seastar::this_shard_id(), connectionId};
    }

    seastar::future<> HandlePacket(const char* data, std::size_t size)
    {
        ClientEnvelope envelope;
        if (!envelope.ParseFromArray(data, static_cast<int>(size)) || !envelope.has_request() ||
            envelope.request().request_id().empty()) {
            LOG_WARN(logger, "[gateway/client-read] 拒绝无效请求信封, connection_id: {}, bytes: {}", connectionId,
                     size);
            co_return;
        }

        TcpPacket request = envelope.request().packet();
        const auto operation = envelope.request().operation();
        const std::string requestId = envelope.request().request_id();
        if (operation == protocol::Operation::OP_UNSPECIFIED || !TcpRequestMatchesOperation(operation, request)) {
            SendError(requestId, operation, ErrorCodes::ProtobufPacket, "invalid protobuf packet");
            co_return;
        }
        if (userId <= 0 && operation != protocol::Operation::OP_LOGIN) {
            SendError(requestId, operation, ErrorCodes::AuthenticationRequired, "gateway session is not authenticated");
            co_return;
        }

        if (operation != protocol::Operation::OP_LOGIN && !(co_await RefreshLeaseIfNeeded()))
            co_return;
        if (co_await DirectProcess(operation, requestId, request))
            co_return;

        const auto rateNow = std::chrono::steady_clock::now();
        if (rateNow - rateWindowStarted >= std::chrono::seconds(1)) {
            rateWindowStarted = rateNow;
            commandsInRateWindow = 0;
        }
        if (++commandsInRateWindow > 100) {
            SendError(requestId, operation, ErrorCodes::ResourceExhausted, "connection command rate exceeded");
            co_return;
        }
        co_await ForwardProcess(operation, requestId, request);
    }

    seastar::future<bool> DirectProcess(protocol::Operation operation, const std::string& requestId,
                                        const TcpPacket& request)
    {
        if (operation == protocol::Operation::OP_LOGIN) {
            try {
                auto result = co_await executor.Submit(
                    request.ByteSizeLong() + connectionId.size(),
                    [this, target = Target(), request, currentUid = userId]() mutable {
                        return HandleLogin(sessions, std::move(target), std::move(request), currentUid);
                    });
                if (result.error == ErrorCodes::Success) {
                    lease = std::move(result.lease);
                    proxy = std::move(result.proxy);
                    userId = result.response.login_response().uid();
                    lastLeaseRefresh = std::chrono::steady_clock::now();
                    LOG_INFO(logger, "[gateway/login] 登录成功, uid: {}, connection_id: {}, generation: {}", userId,
                             connectionId, lease.generation);
                }
                SendResponse(requestId, operation, std::move(result.response));
            } catch (const ExecutorOverloaded&) {
                SendError(requestId, operation, ErrorCodes::ResourceExhausted, "gateway business queue full");
            } catch (const std::exception& error) {
                LOG_ERROR(logger, "[gateway/login] 登录任务失败, connection_id: {}, error: {}", connectionId,
                          error.what());
                SendError(requestId, operation, ErrorCodes::InternalError, "gateway login failed");
            }
            co_return true;
        }

        if (operation == protocol::Operation::OP_PING) {
            TcpPacket response;
            response.mutable_ping_response();
            SetTcpPacketStatus(response, ErrorCodes::Success);
            SendResponse(requestId, operation, std::move(response));
            co_return true;
        }

        if (operation == protocol::Operation::OP_QUIT) {
            TcpPacket response;
            response.mutable_quit_response();
            SetTcpPacketStatus(response, ErrorCodes::Success);
            SendResponse(requestId, operation, std::move(response));
            closeAfterWrite = true;
            co_return true;
        }

        if (operation == protocol::Operation::OP_ACKNOWLEDGE) {
            const auto& receipt = request.receipt();
            if (receipt.seq() > 0)
                AcknowledgeDelivery(receipt.seq());
            if (receipt.conversation_id() <= 0 || receipt.conversation_seq() <= 0)
                co_return true;
        }
        co_return false;
    }

    seastar::future<bool> RefreshLeaseIfNeeded()
    {
        if (std::chrono::steady_clock::now() - lastLeaseRefresh < std::chrono::seconds(20))
            co_return true;
        try {
            const auto currentLease = lease;
            const bool refreshed = co_await executor.Submit(
                currentLease.deviceId.size() + currentLease.connectionId.size(), [this, currentLease, actor = userId] {
                    return sessions.Refresh(actor, currentLease.deviceId, currentLease);
                });
            if (!refreshed) {
                LOG_WARN(logger, "[gateway/lease-refresh] 连接租约已失效, uid: {}, connection_id: {}", userId,
                         connectionId);
                Close();
                co_return false;
            }
            lastLeaseRefresh = std::chrono::steady_clock::now();
            co_return true;
        } catch (const ExecutorOverloaded&) {
            SendError("", protocol::Operation::OP_UNSPECIFIED, ErrorCodes::ResourceExhausted,
                      "gateway business queue full");
            Close();
        } catch (const std::exception& error) {
            LOG_WARN(logger, "[gateway/lease-refresh] 租约续期失败, uid: {}, error: {}", userId, error.what());
            Close();
        }
        co_return false;
    }

    seastar::future<> ForwardProcess(protocol::Operation operation, const std::string& requestId,
                                     const TcpPacket& request)
    {
        gateway::CommandEnvelope command;
        command.set_request_id(requestId);
        command.set_actor_uid(userId);
        command.set_connection_id(connectionId);
        command.set_connection_generation(lease.generation);
        command.set_actor_device_id(lease.deviceId);
        command.set_operation(operation);
        command.set_conversation_id(TcpPacketConversationId(request));
        command.set_packet(SerializeTcpPacket(request));

        const bool expectResponse = operation != protocol::Operation::OP_ACKNOWLEDGE;
        try {
            const auto commandBytes = command.ByteSizeLong();
            const bool forwarded = co_await executor.Submit(
                commandBytes, [this, command = std::move(command), target = Target(), expectResponse, actor = userId,
                               operation, requestId]() mutable {
                    return messageLinks.ForwardNow(
                        std::move(command), [target = std::move(target), expectResponse, actor, operation,
                                             requestId](const gateway::CommandResult& result) {
                            PostCommandResult(target, result, expectResponse, actor, operation, requestId);
                        });
                });
            if (!forwarded && expectResponse)
                SendError(requestId, operation, ErrorCodes::DependencyUnavailable, "no healthy message stream");
        } catch (const ExecutorOverloaded&) {
            if (expectResponse)
                SendError(requestId, operation, ErrorCodes::ResourceExhausted, "gateway business queue full");
        } catch (const std::exception& error) {
            LOG_ERROR(logger, "[gateway/command-forward] 转发任务失败, request_id: {}, error: {}", requestId,
                      error.what());
            if (expectResponse)
                SendError(requestId, operation, ErrorCodes::DependencyUnavailable, "message forward failed");
        }
    }

    bool QueueFrame(seastar::temporary_buffer<char> frame)
    {
        if (closed || frame.size() > kMaxQueuedWriteBytes - queuedWriteBytes || writeQueue.size() >= kMaxQueuedWrites) {
            LOG_WARN(logger, "[gateway/client-write] 客户端写队列已满, connection_id: {}, entries: {}, bytes: {}",
                     connectionId, writeQueue.size(), queuedWriteBytes);
            Close();
            return false;
        }
        const auto frameBytes = frame.size();
        if (!queueBudget.TryAcquireWrite(frameBytes)) {
            LOG_WARN(logger, "[gateway/client-write] 进程写队列预算已满, connection_id: {}", connectionId);
            Close();
            return false;
        }
        queuedWriteBytes += frameBytes;
        try {
            writeQueue.push_back(std::move(frame));
        } catch (...) {
            queuedWriteBytes -= frameBytes;
            queueBudget.ReleaseWrite(frameBytes);
            throw;
        }
        if (!writeActive) {
            writeActive = true;
            auto self = shared_from_this();
            (void)seastar::with_gate(writeGate, [self] {
                return self->WriteLoop();
            }).handle_exception([self](std::exception_ptr error) {
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& exception) {
                    LOG_WARN(logger, "[gateway/client-write] 写入客户端失败, connection_id: {}, error: {}",
                             self->connectionId, exception.what());
                }
                self->Close();
            });
        }
        return true;
    }

    seastar::future<> WriteLoop()
    {
        while (!writeQueue.empty() && !closed) {
            auto frame = std::move(writeQueue.front());
            writeQueue.pop_front();
            const auto bytes = frame.size();
            auto release = seastar::defer([this, bytes] noexcept {
                queuedWriteBytes -= bytes;
                queueBudget.ReleaseWrite(bytes);
            });
            co_await output.write(std::move(frame));
            co_await output.flush();
        }
        writeActive = false;
        if (closeAfterWrite && writeQueue.empty())
            Close();
    }

    void ArmReliableWrite(int64_t ackSeq)
    {
        auto found = reliableWrites.find(ackSeq);
        if (found == reliableWrites.end() || closed)
            return;
        auto timer = std::make_unique<seastar::timer<>>();
        timer->set_callback([server = &server, connectionId = connectionId, ackSeq] {
            auto session = server->Find(connectionId);
            if (session)
                session->RetryReliableWrite(ackSeq);
        });
        timer->arm(std::chrono::seconds(5));
        found->second.timer = std::move(timer);
    }

    void RetryReliableWrite(int64_t ackSeq)
    {
        auto found = reliableWrites.find(ackSeq);
        if (found == reliableWrites.end() || closed)
            return;
        if (found->second.attempts >= 3) {
            reliableWriteBytes -= found->second.envelope.size();
            queueBudget.ReleaseReliable(found->second.envelope.size());
            reliableWrites.erase(found);
            return;
        }
        const auto envelope = found->second.envelope;
        ++found->second.attempts;
        if (!SendRaw(envelope))
            return;
        ArmReliableWrite(ackSeq);
    }

    void AcknowledgeDelivery(int64_t ackSeq)
    {
        auto found = reliableWrites.find(ackSeq);
        if (found == reliableWrites.end())
            return;
        if (found->second.timer)
            found->second.timer->cancel();
        reliableWriteBytes -= found->second.envelope.size();
        queueBudget.ReleaseReliable(found->second.envelope.size());
        reliableWrites.erase(found);
    }

    void SendResponse(const std::string& requestId, protocol::Operation operation, TcpPacket packet)
    {
        ClientEnvelope envelope;
        envelope.mutable_response()->set_request_id(requestId);
        envelope.mutable_response()->set_operation(operation);
        *envelope.mutable_response()->mutable_packet() = std::move(packet);
        SendRaw(SerializeClientEnvelope(envelope));
    }

    void SendError(const std::string& requestId, protocol::Operation operation, int error, const std::string& message)
    {
        SendResponse(requestId, operation, MakeErrorPacket(error, message));
    }

    seastar::connected_socket socket;
    seastar::input_stream<char> input;
    seastar::output_stream<char> output;
    SeastarGatewayServer& server;
    SeastarExecutor& executor;
    SessionManager& sessions;
    MessageLinkManager& messageLinks;
    GatewayQueueBudget& queueBudget;
    seastar::sharded<SeastarGatewayServer>* shards;
    std::string connectionId;
    int64_t userId{};
    db::SessionLease lease;
    std::shared_ptr<ClientSession> proxy;
    bool closed{false};
    bool writeActive{false};
    bool closeAfterWrite{false};
    std::deque<seastar::temporary_buffer<char>> writeQueue;
    seastar::gate writeGate;
    std::size_t queuedWriteBytes{};
    std::unordered_map<int64_t, ReliableWrite> reliableWrites;
    std::size_t reliableWriteBytes{};
    std::chrono::steady_clock::time_point lastLeaseRefresh{};
    std::chrono::steady_clock::time_point rateWindowStarted{std::chrono::steady_clock::now()};
    unsigned int commandsInRateWindow{};
};

bool GatewayQueueBudget::Counter::TryAcquire(std::size_t requestedBytes)
{
    if (requestedBytes > maxBytes)
        return false;

    auto currentItems = items.load(std::memory_order_relaxed);
    do {
        if (currentItems >= maxItems)
            return false;
    } while (!items.compare_exchange_weak(currentItems, currentItems + 1, std::memory_order_acq_rel));

    auto currentBytes = bytes.load(std::memory_order_relaxed);
    do {
        if (currentBytes > maxBytes - requestedBytes) {
            items.fetch_sub(1, std::memory_order_release);
            return false;
        }
    } while (!bytes.compare_exchange_weak(currentBytes, currentBytes + requestedBytes, std::memory_order_acq_rel));
    return true;
}

bool GatewayQueueBudget::Counter::TryResize(std::size_t oldBytes, std::size_t newBytes)
{
    if (newBytes <= oldBytes) {
        bytes.fetch_sub(oldBytes - newBytes, std::memory_order_release);
        return true;
    }

    const auto additional = newBytes - oldBytes;
    auto currentBytes = bytes.load(std::memory_order_relaxed);
    do {
        if (additional > maxBytes - currentBytes)
            return false;
    } while (!bytes.compare_exchange_weak(currentBytes, currentBytes + additional, std::memory_order_acq_rel));
    return true;
}

void GatewayQueueBudget::Counter::Release(std::size_t releasedBytes)
{
    bytes.fetch_sub(releasedBytes, std::memory_order_release);
    items.fetch_sub(1, std::memory_order_release);
}

SeastarGatewayServer::SeastarGatewayServer(SeastarExecutor& executor, SessionManager& sessions,
                                           MessageLinkManager& messageLinks, GatewayQueueBudget& queueBudget,
                                           seastar::sharded<SeastarGatewayServer>* shards, unsigned short port)
    : executor(executor), sessions(sessions), messageLinks(messageLinks), queueBudget(queueBudget), shards(shards),
      port(port)
{
}

seastar::future<> SeastarGatewayServer::Start()
{
    seastar::listen_options options;
    options.reuse_address = true;
    options.listen_backlog = 1024;
    listener.emplace(seastar::listen(seastar::socket_address(seastar::ipv4_addr(port)), options));
    acceptTask = AcceptLoop();
    co_return;
}

seastar::future<> SeastarGatewayServer::Stop()
{
    stopping = true;
    if (listener)
        listener->abort_accept();
    for (auto& [_, session] : connections)
        session->Close();
    try {
        co_await std::move(acceptTask);
    } catch (...) {
    }
    while (!connections.empty())
        co_await seastar::sleep(std::chrono::milliseconds(1));
}

seastar::future<> SeastarGatewayServer::AcceptLoop()
{
    while (!stopping) {
        try {
            auto accepted = co_await listener->accept();
            auto session = std::make_shared<SeastarGatewaySession>(std::move(accepted.connection), *this, executor,
                                                                   sessions, messageLinks, queueBudget, shards);
            const auto connectionId = session->ConnectionId();
            connections.emplace(connectionId, session);
            (void)session->Run().handle_exception([this, session, connectionId](std::exception_ptr error) {
                try {
                    std::rethrow_exception(error);
                } catch (const std::exception& exception) {
                    LOG_ERROR(logger, "[gateway/session] 会话协程异常, connection_id: {}, error: {}",
                              session->ConnectionId(), exception.what());
                }
                session->Close();
                Erase(connectionId);
            });
        } catch (const std::exception& error) {
            if (!stopping)
                LOG_WARN(logger, "[gateway/accept] 接收客户端连接失败, shard: {}, error: {}", seastar::this_shard_id(),
                         error.what());
        }
    }
}

bool SeastarGatewayServer::SendRaw(const std::string& connectionId, std::string envelope)
{
    auto session = Find(connectionId);
    return session && session->SendRaw(std::move(envelope));
}

bool SeastarGatewayServer::SendReliable(const std::string& connectionId, std::string envelope, int64_t ackSeq)
{
    auto session = Find(connectionId);
    return session && session->SendReliable(std::move(envelope), ackSeq);
}

void SeastarGatewayServer::Close(const std::string& connectionId)
{
    if (auto session = Find(connectionId))
        session->Close();
}

void SeastarGatewayServer::DeliverCommandResult(const std::string& connectionId, std::string result,
                                                bool expectResponse, int64_t actor, int operation,
                                                std::string requestId)
{
    auto session = Find(connectionId);
    if (!session)
        return;
    gateway::CommandResult commandResult;
    if (!commandResult.ParseFromString(result))
        return;
    session->HandleForwardResult(commandResult, expectResponse, actor, static_cast<protocol::Operation>(operation),
                                 requestId);
}

void SeastarGatewayServer::Erase(const std::string& connectionId) { connections.erase(connectionId); }

std::shared_ptr<SeastarGatewaySession> SeastarGatewayServer::Find(const std::string& connectionId)
{
    auto found = connections.find(connectionId);
    return found == connections.end() ? nullptr : found->second;
}

} // namespace wimi::connection
