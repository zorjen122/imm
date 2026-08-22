#include "SessionManager.h"

#include "GatewaySession.h"
#include "Logger.h"
#include "TcpMessageCodec.h"

#include <utility>

namespace wimi::connection
{

SessionManager::SessionManager(std::string gatewayId, std::string instanceId, long leaseTtlSeconds)
    : gatewayId(std::move(gatewayId)), instanceId(std::move(instanceId)), leaseTtlSeconds(leaseTtlSeconds)
{
}

db::SessionLease SessionManager::Bind(int64_t uid, const std::string& deviceId,
                                      const std::shared_ptr<GatewaySession>& session)
{
    db::SessionLease lease;
    lease.deviceId = deviceId;
    lease.gatewayId = gatewayId;
    lease.instanceId = instanceId;
    lease.connectionId = session->ConnectionId();
    lease.generation = db::RedisDao::GetInstance()->bindSessionLease(uid, deviceId, gatewayId, instanceId,
                                                                     lease.connectionId, leaseTtlSeconds);
    if (lease.generation <= 0) {
        LOG_ERROR(logger, "[gateway/session-bind] 发布会话失败, uid: {}, connection_id: {}", uid, lease.connectionId);
        return {};
    }

    // rebind 会话，如果旧的会话存在，目前的规则是直接关闭旧的会话（没有明确通知）
    std::shared_ptr<GatewaySession> oldSession;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto& userSessions = sessions[uid];
        auto found = userSessions.find(deviceId);
        if (found != userSessions.end())
            oldSession = found->second.session.lock();
        userSessions[deviceId] = LocalSession{session, lease};
    }
    if (oldSession && oldSession != session) {
        LOG_INFO(logger,
                 "[gateway/session-bind] 新连接替换同设备旧连接, uid: {}, old_connection_id: {}, "
                 "connection_id: {}, generation: {}",
                 uid, oldSession->ConnectionId(), lease.connectionId, lease.generation);
        oldSession->Close();
    } else {
        LOG_INFO(logger, "[gateway/session-bind] 会话已绑定, uid: {}, connection_id: {}, generation: {}", uid,
                 lease.connectionId, lease.generation);
    }
    return lease;
}

bool SessionManager::Refresh(int64_t uid, const std::string& deviceId, const db::SessionLease& lease)
{
    return db::RedisDao::GetInstance()->refreshSessionLease(uid, deviceId, lease, leaseTtlSeconds);
}

void SessionManager::Remove(int64_t uid, const std::string& deviceId, const std::shared_ptr<GatewaySession>& session,
                            const db::SessionLease& lease)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto found = sessions.find(uid);
        if (found == sessions.end())
            return;
        auto device = found->second.find(deviceId);
        if (device == found->second.end())
            return;
        auto current = device->second.session.lock();
        if (current && current != session)
            return;
        found->second.erase(device);
        if (found->second.empty())
            sessions.erase(found);
    }
    db::RedisDao::GetInstance()->clearSessionLease(uid, deviceId, lease);
    LOG_INFO(logger, "[gateway/session-remove] 会话已移除, uid: {}, connection_id: {}, generation: {}", uid,
             lease.connectionId, lease.generation);
}

gateway::ClientForwardStatus SessionManager::Forward(const gateway::ClientForwardEnvelope& forward)
{
    std::shared_ptr<GatewaySession> session;
    db::SessionLease lease;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto user = sessions.find(forward.recipient_uid());
        if (user == sessions.end()) {
            LOG_DEBUG(logger, "[gateway/client-forward] 接收用户不在线, uid: {}, forward_id: {}",
                      forward.recipient_uid(), forward.forward_id());
            return gateway::CLIENT_FORWARD_STATUS_OFFLINE;
        }
        auto found = user->second.find(forward.recipient_device_id());
        if (found == user->second.end()) {
            LOG_DEBUG(logger, "[gateway/client-forward] 接收设备不在线, uid: {}, forward_id: {}",
                      forward.recipient_uid(), forward.forward_id());
            return gateway::CLIENT_FORWARD_STATUS_OFFLINE;
        }
        session = found->second.session.lock();
        lease = found->second.lease;
    }

    if (!session) {
        LOG_DEBUG(logger, "[gateway/client-forward] 本地会话已失效, uid: {}, forward_id: {}", forward.recipient_uid(),
                  forward.forward_id());
        return gateway::CLIENT_FORWARD_STATUS_OFFLINE;
    }

    /*
      Message Core 查询在线 lease 后，把它当时看到的连接身份写入
      ClientForwardEnvelope。Gateway 在真正写 socket 前，要求本地当前 session 的
      connectionId 和 generation 都完全一致；不一致就返回 STALE_ROUTE，不投递。
      未来可以考虑投递，不然导致了一个用户的消息（如通知）无法及时收到，只因为它的连接身份变了。
      目前一般来说，对于可持久化消息是不会丢的，它存放在 Mysql 存储表中。
    */
    if (lease.connectionId != forward.expected_connection_id() ||
        lease.generation != forward.expected_connection_generation()) {
        LOG_DEBUG(
            logger,
            "[gateway/client-forward] 路由已过期, uid: {}, forward_id: {}, generation: {}, expected_generation: {}",
            forward.recipient_uid(), forward.forward_id(), lease.generation, forward.expected_connection_generation());
        return gateway::CLIENT_FORWARD_STATUS_STALE_ROUTE;
    }
    TcpPacket packet;
    const int64_t transportSequence =
        ParseTcpPacket(forward.packet(), packet) ? TcpPacketReliableSequence(forward.protocol_id(), packet) : 0;
    const bool queued = transportSequence > 0
                            ? session->SendReliable(forward.packet(), forward.protocol_id(), transportSequence)
                            : session->SendRaw(forward.packet(), forward.protocol_id());
    if (!queued) {
        LOG_WARN(logger, "[gateway/client-forward] 客户端写队列繁忙, uid: {}, forward_id: {}, protocol_id: {}",
                 forward.recipient_uid(), forward.forward_id(), forward.protocol_id());
        return gateway::CLIENT_FORWARD_STATUS_BACKPRESSURED;
    }
    LOG_TRACE(logger, "[gateway/client-forward] 消息已进入客户端写队列, uid: {}, forward_id: {}",
              forward.recipient_uid(), forward.forward_id());
    return gateway::CLIENT_FORWARD_STATUS_QUEUED;
}

const std::string& SessionManager::GatewayId() const { return gatewayId; }

const std::string& SessionManager::InstanceId() const { return instanceId; }

} // namespace wimi::connection
