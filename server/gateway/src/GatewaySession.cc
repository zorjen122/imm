#include "GatewaySession.h"

#include "Const.h"
#include "DbGlobal.h"
#include "Logger.h"
#include "MessageLinkManager.h"
#include "Mysql.h"
#include "SessionManager.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstring>
#include <exception>
#include <utility>

namespace wimi::connection
{
namespace asio = boost::asio;
namespace
{

constexpr std::size_t kMaxQueuedWrites = PROTOCOL_QUEUE_MAX_SIZE;
std::string NewUuid() { return boost::uuids::to_string(boost::uuids::random_generator{}()); }

std::string ServiceName(uint32_t protocolId)
{
    const auto found = serviceIDMap.find(static_cast<int>(protocolId));
    return found == serviceIDMap.end() ? "UNKNOWN_SERVICE" : found->second;
}

std::shared_ptr<std::string> EncodeFrame(uint32_t protocolId, const std::string& payload)
{
    auto frame = std::make_shared<std::string>();
    frame->resize(PROTOCOL_HEADER_TOTAL + payload.size());
    const uint32_t wireId = htonl(protocolId);
    const uint32_t wireSize = htonl(static_cast<uint32_t>(payload.size()));
    std::memcpy(frame->data(), &wireId, sizeof(wireId));
    std::memcpy(frame->data() + sizeof(wireId), &wireSize, sizeof(wireSize));
    std::memcpy(frame->data() + PROTOCOL_HEADER_TOTAL, payload.data(), payload.size());
    return frame;
}

} // namespace

GatewaySession::GatewaySession(asio::ip::tcp::socket socket, SessionManager& registry, MessageLinkManager& messageLinks,
                               asio::thread_pool& businessPool)
    : socket(std::move(socket)), strand(asio::make_strand(this->socket.get_executor())), registry(registry),
      messageLinks(messageLinks), businessPool(businessPool), connectionId(NewUuid())
{
}

void GatewaySession::Start()
{
    LOG_DEBUG(logger, "[gateway/session] 客户端连接已建立, connection_id: {}", connectionId);
    auto self = shared_from_this();
    asio::co_spawn(strand, Run(), [self](std::exception_ptr error) {
        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& exception) {
                LOG_ERROR(logger, "[gateway/session] 会话协程异常, connection_id: {}, error: {}", self->connectionId,
                          exception.what());
            }
        }
        self->CloseInContext();
    });
}

void GatewaySession::Close()
{
    auto self = shared_from_this();
    asio::post(strand, [self]() { self->CloseInContext(); });
}

bool GatewaySession::SendRaw(std::string packet, uint32_t protocolId)
{
    if (closed.load(std::memory_order_acquire))
        return false;
    const auto queued = queuedWrites.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (queued > kMaxQueuedWrites) {
        queuedWrites.fetch_sub(1, std::memory_order_acq_rel);
        LOG_WARN(logger, "[gateway/client-write] 客户端写队列已满, connection_id: {}, protocol_id: {}, queued: {}",
                 connectionId, protocolId, queued);
        Close();
        return false;
    }

    auto frame = EncodeFrame(protocolId, packet);
    auto self = shared_from_this();
    asio::post(strand, [self, frame = std::move(frame)]() mutable {
        if (self->closed.load(std::memory_order_acquire)) {
            self->queuedWrites.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }
        self->writeQueue.push_back(std::move(frame));
        if (!self->writeActive) {
            self->writeActive = true;
            asio::co_spawn(self->strand, self->WriteLoop(), asio::detached);
        }
    });
    return true;
}

bool GatewaySession::SendReliable(std::string packet, uint32_t protocolId, int64_t ackSeq)
{
    if (ackSeq <= 0)
        return SendRaw(std::move(packet), protocolId);
    if (!SendRaw(packet, protocolId))
        return false;
    auto self = shared_from_this();
    asio::post(strand, [self, packet = std::move(packet), protocolId, ackSeq]() {
        auto found = self->reliableWrites.find(ackSeq);
        if (found != self->reliableWrites.end() && found->second.timer)
            found->second.timer->cancel();
        self->reliableWrites[ackSeq] = ReliableWrite{std::move(packet), protocolId, 1, {}};
        self->ArmReliableWrite(ackSeq);
    });
    return true;
}

const std::string& GatewaySession::ConnectionId() const { return connectionId; }

asio::awaitable<void> GatewaySession::Run()
{
    boost::system::error_code ec;
    while (!closed.load(std::memory_order_acquire)) {
        std::array<uint32_t, 2> header{};
        co_await asio::async_read(socket, asio::buffer(header), asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            if (ec != asio::error::eof && ec != asio::error::operation_aborted)
                LOG_WARN(logger, "[gateway/client-read] 读取协议头失败, connection_id: {}, error: {}", connectionId,
                         ec.message());
            break;
        }

        const uint32_t protocolId = ntohl(header[0]);
        const uint32_t bodySize = ntohl(header[1]);
        if (bodySize > PROTOCOL_RECV_MSS) {
            LOG_WARN(logger,
                     "[gateway/client-read] 拒绝超大数据帧, connection_id: {}, protocol_id: {}, bytes: {}, limit: {}",
                     connectionId, protocolId, bodySize, PROTOCOL_RECV_MSS);
            break;
        }

        std::string payload(bodySize, '\0');
        co_await asio::async_read(socket, asio::buffer(payload), asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            if (ec != asio::error::eof && ec != asio::error::operation_aborted)
                LOG_WARN(logger, "[gateway/client-read] 读取数据帧失败, connection_id: {}, error: {}", connectionId,
                         ec.message());
            break;
        }
        co_await HandlePacket(protocolId, std::move(payload));
    }

    CloseInContext();
    const int64_t uid = userId.load(std::memory_order_acquire);
    if (uid > 0 && !lease.empty()) {
        auto self = shared_from_this();
        co_await asio::co_spawn(
            businessPool,
            [this, self, uid]() -> asio::awaitable<void> {
                registry.Remove(uid, lease.deviceId, self, lease);
                co_return;
            },
            asio::use_awaitable);
    }
}

asio::awaitable<void> GatewaySession::HandlePacket(uint32_t protocolId, std::string payload)
{
    // 所有客户端包先统一完成 Protobuf 解码；失败包不能进入鉴权或 Message Core。
    TcpPacket request;
    if (!ParseTcpPacket(payload, request)) {
        LOG_WARN(logger,
                 "[gateway/client-read] 拒绝无法解析的数据包, connection_id: {}, "
                 "protocol_id: {}, service: {}, bytes: {}",
                 connectionId, protocolId, ServiceName(protocolId), payload.size());
        SendError(protocolId, ErrorCodes::JsonParser, "invalid protobuf packet");
        co_return;
    }

    if (!TcpRequestMatchesService(protocolId, request) || TcpPacketRequestId(request).empty()) {
        LOG_WARN(logger,
                 "[gateway/client-read] 拒绝协议不匹配的数据包, connection_id: {}, "
                 "protocol_id: {}, service: {}, bytes: {}",
                 connectionId, protocolId, ServiceName(protocolId), payload.size());
        SendError(protocolId, ErrorCodes::ProtobufPacket, "invalid protobuf packet");
        co_return;
    }

    const int64_t actor = userId.load(std::memory_order_acquire);
    if (actor <= 0 && protocolId != ID_LOGIN_INIT_REQ) {
        LOG_WARN(logger,
                 "[gateway/command] 拒绝未登录请求, connection_id: {}, "
                 "protocol_id: {}, service: {}",
                 connectionId, protocolId, ServiceName(protocolId));
        SendError(protocolId, ErrorCodes::AuthenticationRequired, "gateway session is not authenticated");
        co_return;
    }

    /*
      活跃连接每 20 秒至多刷新一次 lease 租约，失败表示该连接已被新连接取代。
      采用这种手法可以在处理假死连接的同时，可以拒绝旧连接再发请求，保证唯一性。
      所以后续 gateway 与 client
      没有双向心跳机制，只有单向，因为目前实现覆盖了这点。
    */
    if (protocolId != ID_LOGIN_INIT_REQ) {
        const auto refreshed = co_await RefreshLeaseIfNeeded(actor);
        if (!refreshed)
            co_return;
    }

    if (co_await DirectProcess(protocolId, request, actor))
        co_return;

    // 对所有需要进入 Message Core 的命令执行连接级固定窗口限流。
    // 目前这种超过一秒则重置窗口的局部策略仅作为一个功能点的示例
    const auto rateNow = std::chrono::steady_clock::now();
    if (rateNow - rateWindowStarted >= std::chrono::seconds(1)) {
        rateWindowStarted = rateNow;
        commandsInRateWindow = 0;
    }
    if (++commandsInRateWindow > 100) {
        LOG_WARN(logger,
                 "[gateway/rate-limit] 请求频率超过限制, uid: {}, connection_id: {}, "
                 "protocol_id: {}, service: {}, commands_in_window: {}",
                 actor, connectionId, protocolId, ServiceName(protocolId), commandsInRateWindow);
        SendError(protocolId, ErrorCodes::ResourceExhausted, "connection command rate exceeded");
        co_return;
    }

    co_await ForwardProcess(protocolId, request, actor);
}

asio::awaitable<bool> GatewaySession::DirectProcess(uint32_t protocolId, const TcpPacket& request, int64_t actor)
{
    if (protocolId == ID_LOGIN_INIT_REQ) {
        const auto& login = request.login_request();
        const int64_t claimedUid = login.uid();
        const bool initProfile = login.init_profile();
        LOG_DEBUG(logger, "[gateway/login] 开始校验登录, connection_id: {}, uid: {}, init_profile: {}", connectionId,
                  claimedUid, initProfile);
        auto result = co_await asio::co_spawn(
            businessPool,
            [this, self = shared_from_this(), request]() mutable -> asio::awaitable<ResultConnecctionRoute> {
                co_return RequestConnectionRoute(std::move(request));
            },
            asio::use_awaitable);
        if (result.error == ErrorCodes::Success) {
            lease = result.lease;
            userId.store(result.response.login_response().uid(), std::memory_order_release);
            lastLeaseRefresh = std::chrono::steady_clock::now();
            LOG_INFO(logger, "[gateway/login] 登录成功, uid: {}, connection_id: {}, generation: {}",
                     result.response.login_response().uid(), connectionId, lease.generation);
        } else {
            LOG_WARN(logger, "[gateway/login] 登录失败, connection_id: {}, uid: {}, error: {}", connectionId,
                     claimedUid, result.error);
        }
        if (!SendRaw(SerializeTcpPacket(result.response), ID_LOGIN_INIT_RSP))
            LOG_WARN(logger, "[gateway/login] 登录响应未进入写队列, connection_id: {}", connectionId);
        co_return true;
    }

    // PING 只维持客户端物理连接，Gateway 本地立即回复，不占用 Message 流。
    if (protocolId == ID_PING_REQ) {
        LOG_TRACE(logger, "[gateway/ping] 收到心跳, uid: {}, connection_id: {}", actor, connectionId);
        TcpPacket pong;
        pong.mutable_ping_response();
        SetTcpPacketStatus(pong, ErrorCodes::Success);
        SendRaw(SerializeTcpPacket(pong), ID_PING_RSP);
        co_return true;
    }

    // QUIT 先排队成功响应，再由 WriteLoop 在队列排空后关闭 socket。
    if (protocolId == ID_USER_QUIT_REQ) {
        LOG_INFO(logger, "[gateway/quit] 用户主动退出, uid: {}, connection_id: {}", actor, connectionId);
        TcpPacket response;
        response.mutable_quit_response();
        SetTcpPacketStatus(response, ErrorCodes::Success);
        SendRaw(SerializeTcpPacket(response), ID_USER_QUIT_RSP);
        closeAfterWrite = true;
        co_return true;
    }

    // DELIVERED/READ ACK 都会取消 Gateway 本地重传；没有会话游标的通知 ACK
    // 到此结束，有会话游标的消息 ACK 再转发给 Message Core 更新状态。
    if (protocolId == ID_ACK) {
        const auto& receipt = request.receipt();
        if (receipt.seq() > 0) {
            AcknowledgeDelivery(receipt.seq());
            LOG_TRACE(logger,
                      "[gateway/delivery-ack] 已确认客户端投递, uid: {}, connection_id: {}, "
                      "seq: {}, receipt_type: {}",
                      actor, connectionId, receipt.seq(), static_cast<int>(receipt.receipt_type()));
        }
        if (receipt.conversation_id() <= 0 || receipt.conversation_seq() <= 0)
            co_return true;
    }

    co_return false;
}

asio::awaitable<void> GatewaySession::ForwardProcess(uint32_t protocolId, const TcpPacket& request, int64_t actor)
{
    // 通用业务包统一封装 CommandEnvelope；request_id 负责跨流关联，连接
    // generation 负责在 Message 节点拒绝已经被新登录替代的旧连接命令。
    gateway::CommandEnvelope command;
    command.set_request_id(TcpPacketRequestId(request));
    command.set_actor_uid(actor);
    command.set_connection_id(connectionId);
    command.set_connection_generation(lease.generation);
    command.set_actor_device_id(lease.deviceId);
    command.set_service_id(protocolId);
    command.set_conversation_id(TcpPacketConversationId(request));
    command.set_packet(SerializeTcpPacket(request));

    const bool expectResponse = protocolId != ID_ACK;
    const std::string requestId = command.request_id();
    const int64_t conversationId = command.conversation_id();
    LOG_DEBUG(logger,
              "[gateway/command-forward] 正在转发请求, request_id: {}, uid: {}, "
              "connection_id: {}, generation: {}, protocol_id: {}, service: {}, "
              "conversation_id: {}, expect_response: {}",
              requestId, actor, connectionId, lease.generation, protocolId, ServiceName(protocolId), conversationId,
              expectResponse);
    auto weak = weak_from_this();
    if (!(co_await messageLinks.Forward(std::move(command), [weak, expectResponse, actor, protocolId,
                                                             requestId](const gateway::CommandResult& result) {
            if (auto session = weak.lock()) {
                session->HandleForwardResult(result, expectResponse, actor, protocolId, requestId);
            }
        }))) {
        LOG_WARN(logger,
                 "[gateway/command-forward] 请求转发失败, request_id: {}, uid: {}, "
                 "protocol_id: {}, service: {}, healthy_message_streams: {}",
                 requestId, actor, protocolId, ServiceName(protocolId), messageLinks.HealthyLinkCount());
        if (expectResponse)
            SendError(protocolId, ErrorCodes::DependencyUnavailable, "no healthy message stream");
    }
}

asio::awaitable<bool> GatewaySession::RefreshLeaseIfNeeded(int64_t actor)
{
    if (std::chrono::steady_clock::now() - lastLeaseRefresh < std::chrono::seconds(20))
        co_return true;

    const bool refreshed = co_await asio::co_spawn(
        businessPool,
        [this, actor, currentLease = lease]() -> asio::awaitable<bool> {
            co_return registry.Refresh(actor, currentLease.deviceId, currentLease);
        },
        asio::use_awaitable);
    if (!refreshed) {
        LOG_WARN(logger,
                 "[gateway/lease-refresh] 连接租约已失效, uid: {}, "
                 "connection_id: {}, generation: {}",
                 actor, connectionId, lease.generation);
        CloseInContext();
        co_return false;
    }

    lastLeaseRefresh = std::chrono::steady_clock::now();
    LOG_DEBUG(logger,
              "[gateway/lease-refresh] 连接租约已续期, uid: {}, connection_id: {}, "
              "generation: {}",
              actor, connectionId, lease.generation);
    co_return true;
}

void GatewaySession::HandleForwardResult(const gateway::CommandResult& result, bool expectResponse, int64_t actor,
                                         uint32_t protocolId, const std::string& requestId)
{
    if (result.error() == ErrorCodes::AuthenticationRequired) {
        LOG_WARN(logger,
                 "[gateway/command-result] Message 节点拒绝了过期连接, request_id: {}, uid: {}, "
                 "protocol_id: {}, service: {}",
                 requestId, actor, protocolId, ServiceName(protocolId));
        Close();
        return;
    }
    if (!expectResponse) {
        LOG_DEBUG(logger,
                  "[gateway/command-result] 无需响应的请求已完成, "
                  "request_id: {}, uid: {}, protocol_id: {}, error: {}",
                  requestId, actor, protocolId, result.error());
        return;
    }
    LOG_DEBUG(logger,
              "[gateway/command-result] 收到 Message 处理结果, request_id: {}, uid: {}, "
              "protocol_id: {}, response_service_id: {}, error: {}, "
              "retryable: {}",
              requestId, actor, protocolId, result.response_service_id(), result.error(), result.retryable());
    if (!SendRaw(result.packet(), result.response_service_id()))
        LOG_WARN(logger,
                 "[gateway/command-result] 响应未进入客户端写队列, request_id: {}, uid: "
                 "{}, response_service_id: {}",
                 requestId, actor, result.response_service_id());
}

asio::awaitable<void> GatewaySession::WriteLoop()
{
    boost::system::error_code ec;
    while (!writeQueue.empty() && !closed.load(std::memory_order_acquire)) {
        auto frame = writeQueue.front();
        co_await asio::async_write(socket, asio::buffer(*frame), asio::redirect_error(asio::use_awaitable, ec));
        writeQueue.pop_front();
        queuedWrites.fetch_sub(1, std::memory_order_acq_rel);
        if (ec) {
            LOG_WARN(logger, "[gateway/client-write] 写入客户端失败, connection_id: {}, error: {}", connectionId,
                     ec.message());
            CloseInContext();
            break;
        }
    }
    writeActive = false;
    if (closeAfterWrite && writeQueue.empty())
        CloseInContext();
}

GatewaySession::ResultConnecctionRoute GatewaySession::RequestConnectionRoute(TcpPacket request)
{
    ResultConnecctionRoute result;
    const auto& login = request.login_request();
    const int64_t uid = login.uid();
    if (uid <= 0 || login.device_id().empty() || login.device_id().size() > 36 || login.auth_token().empty() ||
        !db::RedisDao::GetInstance()->validateChatAuthToken(uid, login.auth_token())) {
        result.error = ErrorCodes::TokenInvalid;
        result.response = MakeErrorPacket(ErrorCodes::TokenInvalid, "invalid or expired connection token");
        return result;
    }
    const auto current = userId.load(std::memory_order_acquire);
    if (current > 0 && current != uid) {
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
            // 首次登录响应丢失后客户端可能携带 init=true 重连；此时复用已经
            // 落库的资料，使初始化操作保持幂等。真正的写入失败仍由二次查询识别。
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

    result.lease = registry.Bind(uid, login.device_id(), shared_from_this());
    if (result.lease.empty()) {
        result.error = ErrorCodes::DependencyUnavailable;
        result.response = MakeErrorPacket(ErrorCodes::DependencyUnavailable, "failed to publish session lease");
        return result;
    }

    result.error = ErrorCodes::Success;
    auto* response = result.response.mutable_login_response();
    response->set_uid(uid);
    SetTcpPacketStatus(result.response, ErrorCodes::Success);
    return result;
}

void GatewaySession::CloseInContext()
{
    if (closed.exchange(true))
        return;
    LOG_DEBUG(logger,
              "[gateway/session] 正在关闭客户端连接, uid: {}, connection_id: {}, queued_writes: {}, "
              "pending_acks: {}",
              userId.load(std::memory_order_acquire), connectionId, queuedWrites.load(std::memory_order_acquire),
              reliableWrites.size());
    for (auto& [_, write] : reliableWrites) {
        if (write.timer)
            write.timer->cancel();
    }
    reliableWrites.clear();
    boost::system::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

void GatewaySession::ArmReliableWrite(int64_t ackSeq)
{
    auto found = reliableWrites.find(ackSeq);
    if (found == reliableWrites.end() || closed.load(std::memory_order_acquire))
        return;
    auto timer = std::make_shared<asio::steady_timer>(strand);
    found->second.timer = timer;
    timer->expires_after(std::chrono::seconds(5));
    auto weak = weak_from_this();
    timer->async_wait([weak, ackSeq](const boost::system::error_code& error) {
        if (error)
            return;
        auto self = weak.lock();
        if (!self)
            return;
        auto current = self->reliableWrites.find(ackSeq);
        if (current == self->reliableWrites.end())
            return;
        if (current->second.attempts >= 3) {
            LOG_WARN(logger, "[gateway/delivery-ack] 客户端确认超时, uid: {}, seq: {}, attempts: {}",
                     self->userId.load(std::memory_order_acquire), ackSeq, current->second.attempts);
            self->reliableWrites.erase(current);
            return;
        }
        ++current->second.attempts;
        LOG_DEBUG(logger, "[gateway/delivery-retry] 重发客户端消息, uid: {}, seq: {}, attempt: {}",
                  self->userId.load(std::memory_order_acquire), ackSeq, current->second.attempts);
        if (!self->SendRaw(current->second.packet, current->second.protocolId)) {
            self->reliableWrites.erase(current);
            return;
        }
        self->ArmReliableWrite(ackSeq);
    });
}

void GatewaySession::AcknowledgeDelivery(int64_t ackSeq)
{
    auto found = reliableWrites.find(ackSeq);
    if (found == reliableWrites.end())
        return;
    if (found->second.timer)
        found->second.timer->cancel();
    reliableWrites.erase(found);
}

void GatewaySession::SendError(uint32_t requestId, int error, const std::string& message)
{
    auto response = MakeErrorPacket(error, message);
    SendRaw(SerializeTcpPacket(response), __getServiceResponseId(ServiceID(requestId)));
}

} // namespace wimi::connection
