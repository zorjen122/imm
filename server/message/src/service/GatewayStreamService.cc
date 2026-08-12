#include "GatewayStreamService.h"

#include "Const.h"
#include "Logger.h"
#include "Redis.h"
#include "RequestContext.h"
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

int64_t NowUnixMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

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
            if (finishing || writeQueue.size() >= kMaxStreamQueue)
                return false;
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
        }

        readFrame.Clear();
        StartRead(&readFrame);
    }

    void OnWriteDone(bool ok) override
    {
        if (!ok) {
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
        const uint32_t serviceId = command.service_id();
        if (!registered || requestId.empty()) {
            gateway::MessageToGatewayFrame responseFrame;
            auto* response = responseFrame.mutable_command_result();
            response->set_request_id(requestId);
            response->set_response_service_id(__getServiceResponseId(ServiceID(serviceId)));
            response->set_error(ErrorCodes::AuthenticationRequired);
            response->set_retryable(false);
            response->set_packet(SerializeTcpPacket(MakeErrorPacket(
                ErrorCodes::AuthenticationRequired, "gateway stream must register before sending commands")));
            Enqueue(std::move(responseFrame));
            return;
        }
        if (command.deadline_unix_ms() <= NowUnixMilliseconds()) {
            gateway::MessageToGatewayFrame responseFrame;
            auto* response = responseFrame.mutable_command_result();
            response->set_request_id(requestId);
            response->set_response_service_id(__getServiceResponseId(ServiceID(serviceId)));
            response->set_error(ErrorCodes::DeadlineExceeded);
            response->set_retryable(true);
            response->set_packet(
                SerializeTcpPacket(MakeErrorPacket(ErrorCodes::DeadlineExceeded, "command deadline already elapsed")));
            Enqueue(std::move(responseFrame));
            return;
        }

        // 通过后，不在 gRPC reactor 回调线程里直接跑业务，
        // 而是投递到 message 的后台线程池。
        // gRPC 流线程只负责收发和轻量分发，真正消息服务逻辑在线程池里跑。
        const std::string originGatewayId = gatewayId;
        const std::string originInstanceId = instanceId;
        auto accepted = Service::GetInstance()->PostBackgroundTask(
            [this, originGatewayId, originInstanceId, command = std::move(command)]() mutable {
                HandleBackgroundCommand(originGatewayId, originInstanceId, std::move(command));
            });

        if (!accepted) {
            gateway::MessageToGatewayFrame responseFrame;
            auto* response = responseFrame.mutable_command_result();
            response->set_request_id(requestId);
            response->set_response_service_id(__getServiceResponseId(ServiceID(serviceId)));
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
        const auto remaining = std::chrono::milliseconds(command.deadline_unix_ms() - NowUnixMilliseconds());
        RequestContext context = RequestContext::WithTimeout(
            command.request_id().empty() ? RequestContext::NextRequestId() : command.request_id(),
            getServiceIdString(command.service_id()), RequestSource::Rpc, command.actor_uid(), remaining);
        RequestContextScope contextScope(context);

        TcpPacket packet;
        gateway::MessageToGatewayFrame responseFrame;
        auto* response = responseFrame.mutable_command_result();
        response->set_request_id(command.request_id());
        response->set_response_service_id(__getServiceResponseId(ServiceID(command.service_id())));
        auto reply = [&](int error, bool retryable, std::string errorMessage = {},
                         std::optional<TcpPacket> replyPacket = std::nullopt) {
            if (!replyPacket)
                replyPacket = MakeErrorPacket(error, errorMessage);
            response->set_error(error);
            response->set_retryable(retryable);
            response->set_packet(SerializeTcpPacket(*replyPacket));
            streamService->Reply(originGatewayId, originInstanceId, originReactor, std::move(responseFrame));
        };

        if (context.Expired()) {
            reply(ErrorCodes::DeadlineExceeded, true, "command expired while waiting for a message worker");
            return;
        }

        // 接着查 Redis 里的在线 lease：目的是验证/对齐它和 Gateway
        // 传来的身份完全一致
        // 除了保证连接本身的一致性，也是防旧连接、旧登录、伪造连接继续发命令。
        // 用户重新登录后，旧连接 generation 不匹配，Message 端会拒绝。
        auto actorLease = db::RedisDao::GetInstance()->getSessionLease(command.actor_uid(), command.actor_device_id());
        if (actorLease.empty() || actorLease.gatewayId != originGatewayId ||
            actorLease.instanceId != originInstanceId || actorLease.connectionId != command.connection_id() ||
            actorLease.generation != command.connection_generation()) {
            reply(ErrorCodes::AuthenticationRequired, false, "connection generation is no longer current");
            return;
        }

        // 接着解析原始 TCP 业务包并进入业务层
        // Gateway 传来的 command.packet() 是 TCP packet 序列化结果。
        if (!ParseTcpPacket(command.packet(), packet) || !TcpRequestMatchesService(command.service_id(), packet)) {
            reply(ErrorCodes::JsonParser, false);
            return;
        }

        if (command.service_id() == ID_ACK) {
            auto ack = Service::GetInstance()->Messages().Ack(command.actor_uid(), packet, command.actor_device_id());
            const auto error = TcpPacketError(ack.response);
            reply(error, isRetryableError(error), {}, ack.response);

            if (ack.shouldForwardRead) {
                const auto& receipt = packet.receipt();
                const auto& readReceipt = ack.readReceipt.read_receipt_push();
                gateway::ClientForwardEnvelope forward;
                forward.set_forward_id("read:" + std::to_string(receipt.seq()) + ":" + std::to_string(ack.senderUid));
                forward.set_recipient_uid(ack.senderUid);
                forward.set_protocol_id(ID_TEXT_READ_RECEIPT_NOTIFY);
                forward.set_message_id(receipt.seq());
                forward.set_conversation_id(readReceipt.conversation_id());
                forward.set_conversation_seq(readReceipt.conversation_seq());
                forward.set_packet(SerializeTcpPacket(ack.readReceipt));
                streamService->ForwardToUser(ack.senderUid, std::move(forward));
            }
            return;
        }

        if (command.service_id() == ID_TEXT_SEND_REQ) {
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
                forward.set_protocol_id(ID_TEXT_SEND_REQ);
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

        if (command.service_id() == ID_FILE_SEND_REQ) {
            auto acceptedFile = Service::GetInstance()->Messages().AcceptFile(command.actor_uid(), packet);
            const auto error = TcpPacketError(acceptedFile.response);
            reply(error, isRetryableError(error), {}, acceptedFile.response);
            if (acceptedFile.shouldForward) {
                const auto& accepted = acceptedFile.response.send_message_response();
                gateway::ClientForwardEnvelope forward;
                forward.set_forward_id(std::to_string(accepted.message_id()) + ":" +
                                       std::to_string(acceptedFile.recipientUid));
                forward.set_recipient_uid(acceptedFile.recipientUid);
                forward.set_protocol_id(ID_FILE_SEND_REQ);
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

        if (command.service_id() == ID_GROUP_TEXT_SEND_REQ) {
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
                    forward.set_protocol_id(ID_GROUP_TEXT_SEND_REQ);
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
                senderForward.set_protocol_id(ID_GROUP_TEXT_SEND_REQ);
                senderForward.set_message_id(accepted.message_id());
                senderForward.set_conversation_id(accepted.conversation_id());
                senderForward.set_conversation_seq(accepted.conversation_seq());
                senderForward.set_packet(SerializeTcpPacket(acceptedText.forwardPacket));
                streamService->ForwardToUser(command.actor_uid(), std::move(senderForward), command.actor_device_id());
            }
            return;
        }

        if (command.service_id() == ID_GROUP_FILE_SEND_REQ) {
            auto acceptedFile = Service::GetInstance()->Messages().AcceptGroupFile(command.actor_uid(), packet);
            const auto error = TcpPacketError(acceptedFile.response);
            reply(error, isRetryableError(error), {}, acceptedFile.response);
            if (acceptedFile.shouldForward) {
                const auto& accepted = acceptedFile.response.send_message_response();
                for (const int64_t recipientUid : acceptedFile.recipientUids) {
                    gateway::ClientForwardEnvelope forward;
                    forward.set_forward_id(std::to_string(accepted.message_id()) + ":" + std::to_string(recipientUid));
                    forward.set_recipient_uid(recipientUid);
                    forward.set_protocol_id(ID_GROUP_FILE_SEND_REQ);
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
                senderForward.set_protocol_id(ID_GROUP_FILE_SEND_REQ);
                senderForward.set_message_id(accepted.message_id());
                senderForward.set_conversation_id(accepted.conversation_id());
                senderForward.set_conversation_seq(accepted.conversation_seq());
                senderForward.set_packet(SerializeTcpPacket(acceptedFile.forwardPacket));
                streamService->ForwardToUser(command.actor_uid(), std::move(senderForward), command.actor_device_id());
            }
            return;
        }

        auto packetResponse =
            Service::GetInstance()->ExecuteGatewayCommand(command.service_id(), command.actor_uid(), std::move(packet));
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
    LOG_INFO(netLogger, "Gateway stream registered, gateway: {}, instance: {}", gatewayId, instanceId);
}

void GatewayStreamService::Unregister(const std::string& gatewayId, const std::string& instanceId,
                                      GatewayStreamReactor* reactor)
{
    std::lock_guard<std::mutex> lock(streamsMutex);
    auto found = streams.find(gatewayId);
    if (found == streams.end() || found->second.instanceId != instanceId || found->second.reactor != reactor)
        return;
    streams.erase(found);
    LOG_INFO(netLogger, "Gateway stream unregistered, gateway: {}, instance: {}", gatewayId, instanceId);
}

bool GatewayStreamService::Forward(const db::SessionLease& lease, gateway::ClientForwardEnvelope envelope)
{
    std::lock_guard<std::mutex> lock(streamsMutex);
    auto found = streams.find(lease.gatewayId);
    if (found == streams.end() || found->second.instanceId != lease.instanceId)
        return false;
    gateway::MessageToGatewayFrame frame;
    *frame.mutable_client_forward() = std::move(envelope);
    return found->second.reactor->Enqueue(std::move(frame));
}

bool GatewayStreamService::ForwardToUser(int64_t recipientUid, gateway::ClientForwardEnvelope envelope,
                                         const std::string& excludedDeviceId)
{
    bool forwarded = false;
    auto leases = db::RedisDao::GetInstance()->getSessionLeases(recipientUid);
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
            continue;
        }

        auto refreshed = db::RedisDao::GetInstance()->getSessionLease(recipientUid, lease.deviceId);
        if (refreshed.empty() ||
            (refreshed.gatewayId == lease.gatewayId && refreshed.instanceId == lease.instanceId &&
             refreshed.connectionId == lease.connectionId && refreshed.generation == lease.generation))
            continue;
        target.set_expected_connection_id(refreshed.connectionId);
        target.set_expected_connection_generation(refreshed.generation);
        if (Forward(refreshed, std::move(target)))
            forwarded = true;
    }
    return forwarded;
}

bool GatewayStreamService::Reply(const std::string& gatewayId, const std::string& instanceId,
                                 GatewayStreamReactor* reactor, gateway::MessageToGatewayFrame frame)
{
    std::lock_guard<std::mutex> lock(streamsMutex);
    auto found = streams.find(gatewayId);
    if (found == streams.end() || found->second.instanceId != instanceId || found->second.reactor != reactor)
        return false;
    return reactor->Enqueue(std::move(frame));
}

void GatewayStreamService::DrainAll(const std::string& reason)
{
    draining.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(streamsMutex);
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
