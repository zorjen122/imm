#include "ClientForwardService.h"

#include "Const.h"
#include "GatewayStreamService.h"
#include "Logger.h"
#include "Redis.h"

#include <utility>

namespace wimi
{

void ClientForwardService::SetGatewayStreamService(rpc::GatewayStreamService* service)
{
    gatewayStreamService.store(service, std::memory_order_release);
}

bool ClientForwardService::ForwardToGateway(int64_t uid, const std::string& packet, uint32_t protocolId,
                                            int64_t forwardId, int64_t messageId, int64_t conversationId,
                                            int64_t conversationSeq) const
{
    auto* service = gatewayStreamService.load(std::memory_order_acquire);
    if (!service) {
        LOG_WARN(businessLogger, "[message/client-forward] Gateway 流服务不可用, uid: {}, protocol_id: {}", uid,
                 protocolId);
        return false;
    }
    if (forwardId <= 0)
        forwardId = db::RedisDao::GetInstance()->generateMsgId();
    gateway::ClientForwardEnvelope forward;
    forward.set_forward_id(std::to_string(protocolId) + ":" + std::to_string(forwardId) + ":" + std::to_string(uid));
    forward.set_recipient_uid(uid);
    forward.set_protocol_id(protocolId);
    forward.set_message_id(messageId);
    forward.set_conversation_id(conversationId);
    forward.set_conversation_seq(conversationSeq);
    forward.set_packet(packet);
    const bool forwarded = service->ForwardToUser(uid, std::move(forward));
    LOG_DEBUG(businessLogger, "[message/client-forward] 客户端推送处理完成, uid: {}, protocol_id: {}, forwarded: {}",
              uid, protocolId, forwarded);
    return forwarded;
}

} // namespace wimi
