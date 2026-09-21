#pragma once

#include "Redis.h"
#include "gateway_message.pb.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace wimi::connection
{

class ClientSession;

class SessionManager
{
  public:
    SessionManager(std::string gatewayId, std::string instanceId, long leaseTtlSeconds = 60);

    db::SessionLease Bind(int64_t uid, const std::string& deviceId, const std::shared_ptr<ClientSession>& session);
    bool Refresh(int64_t uid, const std::string& deviceId, const db::SessionLease& lease);
    void Remove(int64_t uid, const std::string& deviceId, const std::shared_ptr<ClientSession>& session,
                const db::SessionLease& lease);
    void Discard(int64_t uid, const std::string& deviceId, const std::shared_ptr<ClientSession>& session);
    gateway::ClientForwardStatus Forward(const gateway::ClientForwardEnvelope& forward);

    const std::string& GatewayId() const;
    const std::string& InstanceId() const;

  private:
    bool RemoveLocal(int64_t uid, const std::string& deviceId, const std::shared_ptr<ClientSession>& session);

    struct LocalSession {
        std::shared_ptr<ClientSession> session;
        db::SessionLease lease;
    };

    std::string gatewayId;  // 配置文件中定义的网关名
    std::string instanceId; // 这次 Gateway 进程启动的 UUID
    long leaseTtlSeconds;
    std::mutex mutex;
    std::unordered_map<int64_t, std::unordered_map<std::string, LocalSession>> sessions;
};

} // namespace wimi::connection
