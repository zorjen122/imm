#include "GatewayStreamService.h"

#include "Const.h"
#include "Logger.h"
#include "Redis.h"
#include "Service.h"
#include "TcpMessageCodec.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <utility>

namespace wimi::rpc
{
namespace
{

constexpr std::size_t kMaxStreamQueue = 4096;

} // namespace

class GatewayStreamReactor final
    : public grpc::ServerBidiReactor<gateway::GatewayToMessageFrame, gateway::MessageToGatewayFrame>
{
  public:
    GatewayStreamReactor(GatewayStreamService& service, std::string authenticatedPeer)
        : service(service), authenticatedPeer(std::move(authenticatedPeer))
    {
        StartRead(&readFrame);
    }

    bool Enqueue(gateway::MessageToGatewayFrame frame)
    {
        bool startWrite = false;
        {
            std::lock_guard<std::mutex> lock(writeMutex);
            if (finishing)
                return false;
            if (writeQueue.size() >= kMaxStreamQueue) {
                LOG_WARN(logger, "[message/gateway-stream] 写队列已满, gateway: {}, queued: {}", gatewayId,
                         writeQueue.size());
                return false;
            }
            writeQueue.push_back(std::move(frame));
            if (!writeInFlight) {
                writeInFlight = true;
                writeFrame = std::move(writeQueue.front());
                writeQueue.pop_front();
                startWrite = true;
            }
        }
        if (startWrite)
            StartWrite(&writeFrame);
        return true;
    }

    void OnReadDone(bool ok) override
    {
        if (!ok) {
            LOG_INFO(logger, "[message/gateway-stream] Gateway 读取通道已关闭, gateway: {}, instance: {}", gatewayId,
                     instanceId);
            FinishOnce();
            return;
        }

        if (readFrame.has_register_gateway()) {
            HandleRegister(readFrame.register_gateway());
        } else if (readFrame.has_command()) {
            HandleCommand(readFrame.command());
        } else if (readFrame.has_heartbeat()) {
            gateway::MessageToGatewayFrame response;
            auto* heartbeat = response.mutable_heartbeat_ack();
            heartbeat->set_sent_at_unix_ms(readFrame.heartbeat().sent_at_unix_ms());
            heartbeat->set_sequence(readFrame.heartbeat().sequence());
            Enqueue(std::move(response));
        } else if (readFrame.has_client_forward_ack()) {
            LOG_DEBUG(logger,
                      "[message/client-forward-ack] 收到 Gateway 推送确认, gateway: {}, forward_id: {}, status: {}",
                      gatewayId, readFrame.client_forward_ack().forward_id(),
                      static_cast<int>(readFrame.client_forward_ack().status()));
        } else {
            LOG_WARN(logger, "[message/gateway-stream] 收到空或未知数据帧, gateway: {}, instance: {}", gatewayId,
                     instanceId);
        }

        readFrame.Clear();
        StartRead(&readFrame);
    }

    void OnWriteDone(bool ok) override
    {
        if (!ok) {
            LOG_WARN(logger, "[message/gateway-stream] Gateway 写入通道已关闭, gateway: {}, instance: {}", gatewayId,
                     instanceId);
            FinishOnce();
            return;
        }
        bool startWrite = false;
        {
            std::lock_guard<std::mutex> lock(writeMutex);
            if (!writeQueue.empty()) {
                writeFrame = std::move(writeQueue.front());
                writeQueue.pop_front();
                startWrite = true;
            } else {
                writeInFlight = false;
            }
        }
        if (startWrite)
            StartWrite(&writeFrame);
    }

    void OnDone() override
    {
        LOG_DEBUG(logger, "[message/gateway-stream] Gateway 流已结束, gateway: {}, instance: {}", gatewayId,
                  instanceId);
        service.Unregister(gatewayId, instanceId, this);
        delete this;
    }

  private:
    void HandleRegister(const gateway::RegisterGateway& request)
    {
        gatewayId = request.gateway_id();
        instanceId = request.instance_id();
        streamEpoch = request.stream_epoch();

        // 更好的方法是在 Redis 里面校验一遍，现在只是判空
        const bool valid = request.protocol_version() == 1 && !gatewayId.empty() && !instanceId.empty() &&
                           !service.Draining() && (!service.RequirePeerIdentity() || authenticatedPeer == gatewayId);
        if (valid) {
            service.Register(gatewayId, instanceId, this);
            registered = true;
        } else {
            LOG_WARN(logger,
                     "[message/gateway-register] 拒绝 Gateway 注册, gateway: {}, instance: {}, "
                     "protocol_version: {}, draining: {}, peer_identity_required: {}",
                     gatewayId, instanceId, request.protocol_version(), service.Draining(),
                     service.RequirePeerIdentity());
        }

        gateway::MessageToGatewayFrame response;
        auto* result = response.mutable_register_result();
        result->set_accepted(valid);
        result->set_message_node_id(service.MessageNodeId());
        result->set_stream_epoch(streamEpoch);
        if (!valid)
            result->set_reason("unsupported protocol or gateway identity does not match mTLS peer");
        Enqueue(std::move(response));
    }

    void HandleCommand(gateway::CommandEnvelope command)
    {
        const std::string requestId = command.request_id();
        const auto operation = command.operation();
        if (!registered || requestId.empty()) {
            LOG_WARN(logger,
                     "[message/command] 拒绝未注册或缺少 request_id 的请求, gateway: {}, operation: {}, "
                     "registered: {}",
                     gatewayId, static_cast<int>(operation), registered);
            gateway::MessageToGatewayFrame responseFrame;
            auto* response = responseFrame.mutable_command_result();
            response->set_request_id(requestId);
            response->set_error(ErrorCodes::AuthenticationRequired);
            response->set_retryable(false);
            response->set_packet(SerializeTcpPacket(MakeErrorPacket(
                ErrorCodes::AuthenticationRequired, "gateway stream must register before sending commands")));
            Enqueue(std::move(responseFrame));
            return;
        }
        // 通过后，不在 gRPC reactor 回调线程里直接跑业务，
        // 而是投递到 message 的后台线程池。
        // gRPC 流线程只负责收发和轻量分发，真正消息服务逻辑在线程池里跑。
        const std::string originGatewayId = gatewayId;
        const std::string originInstanceId = instanceId;
        LOG_DEBUG(logger, "[message/command] 请求已进入业务队列, gateway: {}, request_id: {}, uid: {}, operation: {}",
                  gatewayId, requestId, command.actor_uid(), static_cast<int>(operation));
        auto accepted = Service::GetInstance()->PostBackgroundTask(
            [this, originGatewayId, originInstanceId, command = std::move(command)]() mutable {
                HandleBackgroundCommand(originGatewayId, originInstanceId, std::move(command));
            });

        if (!accepted) {
            LOG_WARN(logger, "[message/command] 业务队列已满, 请求被拒绝, gateway: {}, request_id: {}, operation: {}",
                     gatewayId, requestId, static_cast<int>(operation));
            gateway::MessageToGatewayFrame responseFrame;
            auto* response = responseFrame.mutable_command_result();
            response->set_request_id(requestId);
            response->set_error(ErrorCodes::ResourceExhausted);
            response->set_retryable(true);
            response->set_packet(
                SerializeTcpPacket(MakeErrorPacket(ErrorCodes::ResourceExhausted, "message worker queue is full")));
            Enqueue(std::move(responseFrame));
        }
    }

    void HandleBackgroundCommand(const std::string& originGatewayId, const std::string& originInstanceId,
                                 gateway::CommandEnvelope command)
    {
        auto* originReactor = this;
        auto* streamService = &service;
        TcpPacket packet;
        gateway::MessageToGatewayFrame responseFrame;
        auto* response = responseFrame.mutable_command_result();
        response->set_request_id(command.request_id());
        auto reply = [&](int error, bool retryable, std::string errorMessage = {},
                         std::optional<TcpPacket> replyPacket = std::nullopt) {
            if (!replyPacket)
                replyPacket = MakeErrorPacket(error, errorMessage);
            response->set_error(error);
            response->set_retryable(retryable);
            response->set_packet(SerializeTcpPacket(*replyPacket));
            const bool replied =
                streamService->Reply(originGatewayId, originInstanceId, originReactor, std::move(responseFrame));
            if (!replied) {
                LOG_WARN(logger,
                         "[message/command-result] 处理结果未进入 Gateway 写队列, gateway: {}, request_id: {}, "
                         "operation: {}, error: {}",
                         originGatewayId, command.request_id(), static_cast<int>(command.operation()), error);
            } else {
                LOG_DEBUG(logger,
                          "[message/command-result] 处理结果已返回 Gateway, gateway: {}, request_id: {}, "
                          "operation: {}, error: {}, retryable: {}",
                          originGatewayId, command.request_id(), static_cast<int>(command.operation()), error,
                          retryable);
            }
        };

        // 接着查 Redis 里的在线 lease：目的是验证/对齐它和 Gateway
        // 传来的身份完全一致
        // 除了保证连接本身的一致性，也是防旧连接、旧登录、伪造连接继续发命令。
        // 用户重新登录后，旧连接 generation 不匹配，Message 端会拒绝。
        auto actorLease = db::RedisDao::GetInstance()->getSessionLease(command.actor_uid(), command.actor_device_id());
        if (actorLease.empty() || actorLease.gatewayId != originGatewayId ||
            actorLease.instanceId != originInstanceId || actorLease.connectionId != command.connection_id() ||
            actorLease.generation != command.connection_generation()) {
            LOG_WARN(logger, "[message/command] 连接租约校验失败, gateway: {}, request_id: {}, uid: {}, operation: {}",
                     originGatewayId, command.request_id(), command.actor_uid(), static_cast<int>(command.operation()));
            reply(ErrorCodes::AuthenticationRequired, false, "connection generation is no longer current");
            return;
        }

        // 接着解析原始 TCP 业务包并进入业务层
        // Gateway 传来的 command.packet() 是 TCP packet 序列化结果。
        if (!ParseTcpPacket(command.packet(), packet) || !TcpRequestMatchesOperation(command.operation(), packet)) {
            LOG_WARN(logger,
                     "[message/command] 请求包解析或协议校验失败, gateway: {}, request_id: {}, uid: {}, "
                     "operation: {}, bytes: {}",
                     originGatewayId, command.request_id(), command.actor_uid(), static_cast<int>(command.operation()),
                     command.packet().size());
            reply(ErrorCodes::JsonParser, false);
            return;
        }

        if (command.operation() == protocol::Operation::OP_ACKNOWLEDGE) {
            auto ack = Service::GetInstance()->Messages().Ack(command.actor_uid(), packet, command.actor_device_id());
            const auto error = TcpPacketError(ack.response);
            reply(error, isRetryableError(error), {}, ack.response);

            if (ack.shouldForwardRead) {
                const auto& receipt = packet.receipt();
                const auto& readReceipt = ack.readReceipt.read_receipt_push();
                gateway::ClientForwardEnvelope forward;
                forward.set_forward_id("read:" + std::to_string(receipt.seq()) + ":" + std::to_string(ack.senderUid));
                forward.set_recipient_uid(ack.senderUid);
                forward.set_message_id(receipt.seq());
                forward.set_conversation_id(readReceipt.conversation_id());
                forward.set_conversation_seq(readReceipt.conversation_seq());
                forward.set_packet(SerializeTcpPacket(ack.readReceipt));
                streamService->ForwardToUser(ack.senderUid, std::move(forward));
            }
            return;
        }

        if (command.operation() == protocol::Operation::OP_SEND_TEXT) {
            auto acceptedText = Service::GetInstance()->Messages().AcceptText(command.actor_uid(), packet);
            const auto error = TcpPacketError(acceptedText.response);
            const bool retryable = isRetryableError(error);
            reply(error, retryable, {}, acceptedText.response);

            // 如果不是一个重复消息，则转发到接收者
            if (acceptedText.shouldForward) {
                const auto& accepted = acceptedText.response.send_message_response();
                gateway::ClientForwardEnvelope forward;
                forward.set_forward_id(std::to_string(accepted.message_id()) + ":" +
                                       std::to_string(acceptedText.recipientUid));
                forward.set_recipient_uid(acceptedText.recipientUid);
                forward.set_message_id(accepted.message_id());
                forward.set_conversation_id(accepted.conversation_id());
                forward.set_conversation_seq(accepted.conversation_seq());
                forward.set_packet(SerializeTcpPacket(acceptedText.forwardPacket));
                streamService->ForwardToUser(acceptedText.recipientUid, forward);
                forward.set_forward_id(std::to_string(accepted.message_id()) + ":" +
                                       std::to_string(command.actor_uid()));
                streamService->ForwardToUser(command.actor_uid(), std::move(forward), command.actor_device_id());
            }
            return;
        }

        if (command.operation() == protocol::Operation::OP_SEND_FILE) {
            auto acceptedFile = Service::GetInstance()->Messages().AcceptFile(command.actor_uid(), packet);
            const auto error = TcpPacketError(acceptedFile.response);
            reply(error, isRetryableError(error), {}, acceptedFile.response);
            if (acceptedFile.shouldForward) {
                const auto& accepted = acceptedFile.response.send_message_response();
                gateway::ClientForwardEnvelope forward;
                forward.set_forward_id(std::to_string(accepted.message_id()) + ":" +
                                       std::to_string(acceptedFile.recipientUid));
                forward.set_recipient_uid(acceptedFile.recipientUid);
                forward.set_message_id(accepted.message_id());
                forward.set_conversation_id(accepted.conversation_id());
                forward.set_conversation_seq(accepted.conversation_seq());
                forward.set_packet(SerializeTcpPacket(acceptedFile.forwardPacket));
                streamService->ForwardToUser(acceptedFile.recipientUid, forward);
                forward.set_forward_id(std::to_string(accepted.message_id()) + ":" +
                                       std::to_string(command.actor_uid()));
                streamService->ForwardToUser(command.actor_uid(), std::move(forward), command.actor_device_id());
            }
            return;
        }

        if (command.operation() == protocol::Operation::OP_SEND_GROUP_TEXT) {
            auto acceptedText = Service::GetInstance()->Messages().AcceptGroupText(command.actor_uid(), packet);
            const auto error = TcpPacketError(acceptedText.response);
            const bool retryable = isRetryableError(error);
            reply(error, retryable, {}, acceptedText.response);

            if (acceptedText.shouldForward) {
                const auto& accepted = acceptedText.response.send_message_response();
                for (const int64_t recipientUid : acceptedText.recipientUids) {
                    gateway::ClientForwardEnvelope forward;
                    forward.set_forward_id(std::to_string(accepted.message_id()) + ":" + std::to_string(recipientUid));
                    forward.set_recipient_uid(recipientUid);
                    forward.set_message_id(accepted.message_id());
                    forward.set_conversation_id(accepted.conversation_id());
                    forward.set_conversation_seq(accepted.conversation_seq());
                    forward.set_packet(SerializeTcpPacket(acceptedText.forwardPacket));
                    streamService->ForwardToUser(recipientUid, std::move(forward));
                }
                gateway::ClientForwardEnvelope senderForward;
                senderForward.set_forward_id(std::to_string(accepted.message_id()) + ":" +
                                             std::to_string(command.actor_uid()));
                senderForward.set_recipient_uid(command.actor_uid());
                senderForward.set_message_id(accepted.message_id());
                senderForward.set_conversation_id(accepted.conversation_id());
                senderForward.set_conversation_seq(accepted.conversation_seq());
                senderForward.set_packet(SerializeTcpPacket(acceptedText.forwardPacket));
                streamService->ForwardToUser(command.actor_uid(), std::move(senderForward), command.actor_device_id());
            }
            return;
        }

        if (command.operation() == protocol::Operation::OP_SEND_GROUP_FILE) {
            auto acceptedFile = Service::GetInstance()->Messages().AcceptGroupFile(command.actor_uid(), packet);
            const auto error = TcpPacketError(acceptedFile.response);
            reply(error, isRetryableError(error), {}, acceptedFile.response);
            if (acceptedFile.shouldForward) {
                const auto& accepted = acceptedFile.response.send_message_response();
                for (const int64_t recipientUid : acceptedFile.recipientUids) {
                    gateway::ClientForwardEnvelope forward;
                    forward.set_forward_id(std::to_string(accepted.message_id()) + ":" + std::to_string(recipientUid));
                    forward.set_recipient_uid(recipientUid);
                    forward.set_message_id(accepted.message_id());
                    forward.set_conversation_id(accepted.conversation_id());
                    forward.set_conversation_seq(accepted.conversation_seq());
                    forward.set_packet(SerializeTcpPacket(acceptedFile.forwardPacket));
                    streamService->ForwardToUser(recipientUid, std::move(forward));
                }
                gateway::ClientForwardEnvelope senderForward;
                senderForward.set_forward_id(std::to_string(accepted.message_id()) + ":" +
                                             std::to_string(command.actor_uid()));
                senderForward.set_recipient_uid(command.actor_uid());
                senderForward.set_message_id(accepted.message_id());
                senderForward.set_conversation_id(accepted.conversation_id());
                senderForward.set_conversation_seq(accepted.conversation_seq());
                senderForward.set_packet(SerializeTcpPacket(acceptedFile.forwardPacket));
                streamService->ForwardToUser(command.actor_uid(), std::move(senderForward), command.actor_device_id());
            }
            return;
        }

        auto packetResponse =
            Service::GetInstance()->ExecuteGatewayCommand(command.operation(), command.actor_uid(), std::move(packet));
        const auto error = TcpPacketError(packetResponse);
        reply(error, isRetryableError(error), {}, packetResponse);
    }

    void FinishOnce()
    {
        if (finishing.exchange(true))
            return;
        Finish(grpc::Status::OK);
    }

    GatewayStreamService& service;
    std::string gatewayId;
    std::string instanceId;
    std::string authenticatedPeer;
    uint64_t streamEpoch{0};
    gateway::GatewayToMessageFrame readFrame;
    gateway::MessageToGatewayFrame writeFrame;
    std::mutex writeMutex;
    std::deque<gateway::MessageToGatewayFrame> writeQueue;
    bool writeInFlight{false};
    std::atomic<bool> finishing{false};
    bool registered{false};
};

GatewayStreamService::GatewayStreamService(std::string messageNodeId, bool requirePeerIdentity)
    : messageNodeId(std::move(messageNodeId)), requirePeerIdentity(requirePeerIdentity)
{
}

grpc::ServerBidiReactor<gateway::GatewayToMessageFrame, gateway::MessageToGatewayFrame>*
GatewayStreamService::Connect(grpc::CallbackServerContext* context)
{
    std::string peerIdentity;
    if (auto auth = context->auth_context()) {
        auto identities = auth->GetPeerIdentity();
        if (!identities.empty())
            peerIdentity.assign(identities.front().data(), identities.front().size());
    }
    return new GatewayStreamReactor(*this, std::move(peerIdentity));
}

void GatewayStreamService::Register(const std::string& gatewayId, const std::string& instanceId,
                                    GatewayStreamReactor* reactor)
{
    std::lock_guard<std::mutex> lock(streamsMutex);
    streams[gatewayId] = RegisteredStream{instanceId, reactor};
    LOG_INFO(logger, "[message/gateway-register] Gateway 流已注册, gateway: {}, instance: {}", gatewayId, instanceId);
}

void GatewayStreamService::Unregister(const std::string& gatewayId, const std::string& instanceId,
                                      GatewayStreamReactor* reactor)
{
    std::lock_guard<std::mutex> lock(streamsMutex);
    auto found = streams.find(gatewayId);
    if (found == streams.end() || found->second.instanceId != instanceId || found->second.reactor != reactor)
        return;
    streams.erase(found);
    LOG_INFO(logger, "[message/gateway-unregister] Gateway 流已注销, gateway: {}, instance: {}", gatewayId, instanceId);
}

bool GatewayStreamService::Forward(const db::SessionLease& lease, gateway::ClientForwardEnvelope envelope)
{
    std::lock_guard<std::mutex> lock(streamsMutex);
    auto found = streams.find(lease.gatewayId);
    if (found == streams.end() || found->second.instanceId != lease.instanceId) {
        LOG_DEBUG(logger, "[message/client-forward] Gateway 流不可用, gateway: {}, uid: {}", lease.gatewayId,
                  envelope.recipient_uid());
        return false;
    }
    gateway::MessageToGatewayFrame frame;
    *frame.mutable_client_forward() = std::move(envelope);
    return found->second.reactor->Enqueue(std::move(frame));
}

bool GatewayStreamService::ForwardToUser(int64_t recipientUid, gateway::ClientForwardEnvelope envelope,
                                         const std::string& excludedDeviceId)
{
    bool forwarded = false;
    auto leases = db::RedisDao::GetInstance()->getSessionLeases(recipientUid);
    std::size_t forwardedCount = 0;
    for (const auto& lease : leases) {
        if (lease.deviceId == excludedDeviceId)
            continue;
        auto target = envelope;
        target.set_forward_id(envelope.forward_id() + ":" + lease.deviceId);
        target.set_recipient_uid(recipientUid);
        target.set_recipient_device_id(lease.deviceId);
        target.set_expected_connection_id(lease.connectionId);
        target.set_expected_connection_generation(lease.generation);
        if (Forward(lease, target)) {
            forwarded = true;
            ++forwardedCount;
            continue;
        }

        auto refreshed = db::RedisDao::GetInstance()->getSessionLease(recipientUid, lease.deviceId);
        if (refreshed.empty() ||
            (refreshed.gatewayId == lease.gatewayId && refreshed.instanceId == lease.instanceId &&
             refreshed.connectionId == lease.connectionId && refreshed.generation == lease.generation))
            continue;
        target.set_expected_connection_id(refreshed.connectionId);
        target.set_expected_connection_generation(refreshed.generation);
        if (Forward(refreshed, std::move(target))) {
            forwarded = true;
            ++forwardedCount;
        }
    }
    LOG_DEBUG(logger, "[message/client-forward] 用户推送处理完成, uid: {}, leases: {}, forwarded: {}", recipientUid,
              leases.size(), forwardedCount);
    return forwarded;
}

bool GatewayStreamService::Reply(const std::string& gatewayId, const std::string& instanceId,
                                 GatewayStreamReactor* reactor, gateway::MessageToGatewayFrame frame)
{
    std::lock_guard<std::mutex> lock(streamsMutex);
    auto found = streams.find(gatewayId);
    if (found == streams.end() || found->second.instanceId != instanceId || found->second.reactor != reactor) {
        LOG_DEBUG(logger, "[message/command-result] Gateway 流已变化, 无法返回结果, gateway: {}", gatewayId);
        return false;
    }
    return reactor->Enqueue(std::move(frame));
}

void GatewayStreamService::DrainAll(const std::string& reason)
{
    draining.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(streamsMutex);
    LOG_INFO(logger, "[message/gateway-drain] 正在通知 Gateway 节点下线, streams: {}, reason: {}", streams.size(),
             reason);
    for (const auto& [_, stream] : streams) {
        gateway::MessageToGatewayFrame frame;
        auto* notice = frame.mutable_drain_notice();
        notice->set_message_node_id(messageNodeId);
        notice->set_reason(reason);
        stream.reactor->Enqueue(std::move(frame));
    }
}

const std::string& GatewayStreamService::MessageNodeId() const { return messageNodeId; }

bool GatewayStreamService::RequirePeerIdentity() const { return requirePeerIdentity; }

bool GatewayStreamService::Draining() const { return draining.load(std::memory_order_acquire); }

} // namespace wimi::rpc
