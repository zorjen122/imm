#pragma once

#include <boost/asio/deadline_timer.hpp>
#include <chrono>
#include <memory>
#include <sw/redis++/redis.h>
#include <sw/redis++/sentinel.h>

#include <atomic>

#include "Configer.h"
#include "Const.h"
#include "DbGlobal.h"
#include "Logger.h"
#include "Metrics.h"
#include "RequestContext.h"
#include <condition_variable>
#include <jsoncpp/json/json.h>
#include <iterator>
#include <mutex>
#include <string>
#include <queue>
#include <spdlog/spdlog.h>
#include <thread>
#include <vector>

namespace wimi::db
{

struct SessionLease {
    std::string deviceId;
    std::string gatewayId;
    std::string instanceId;
    std::string connectionId;
    int64_t generation{0};

    bool empty() const
    {
        return deviceId.empty() || gatewayId.empty() || instanceId.empty() || connectionId.empty() || generation <= 0;
    }
};

class RedisPool
{
  public:
    using Ptr = std::shared_ptr<RedisPool>;

    RedisPool(std::shared_ptr<sw::redis::Sentinel> sentinel, const std::string& masterName, const std::string& password,
              size_t poolSize = 2)
        : stopEnable(false), sentinel(std::move(sentinel)), masterName(masterName), password(password)
    {
        for (size_t i = 0; i < poolSize; ++i) {
            try {
                auto session = CreateConnection();
                connections.push(std::move(session));
            } catch (sw::redis::Error& e) {
                LOG_DEBUG(logger, "redis connection is error: {}, continue this connection", e.what());
                continue;
            }
        }

        if (connections.empty()) {
            spdlog::warn("REDIS CONNECTION POOL INIT IS WRONG, NO CONNECTION IS "
                         "SUCCESSFUL, EXIT NOW ");
            return;
        }

        keepThread = std::thread([this]() {
            short count = 0;
            while (!stopEnable) {
                if (count == 60) {
                    keepConnectionHandle();
                    count = 0;
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
                count++;
            }
        });
    }

    ~RedisPool()
    {
        Close();
        std::lock_guard<std::mutex> lock(poolMutex);
        while (!connections.empty()) {
            connections.pop();
        }

        // 等待时间不超1秒钟，因为keepThread线程函数每次休眠1秒
        if (keepThread.joinable())
            keepThread.join();
        LOG_INFO(logger, "RedisPool::~RedisPool()!");
    }

    void ClearConnections()
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        while (!connections.empty()) {
            auto context = std::move(connections.front());
            connections.pop();
        }
    }

    std::unique_ptr<sw::redis::Redis> GetConnection()
    {
        return GetConnectionUntil(RequestContextScope::CurrentDeadlineOr(std::chrono::milliseconds(1000)));
    }

    std::unique_ptr<sw::redis::Redis> GetConnectionUntil(RequestContext::Deadline deadline)
    {
        std::unique_lock<std::mutex> lock(poolMutex);
        bool ready = condVar.wait_until(lock, deadline, [this] {
            if (stopEnable) {
                return true;
            }
            return !connections.empty();
        });
        if (!ready) {
            Metrics::Increment(Metric::RedisAcquireTimeout);
            LOG_WARN(logger, "Redis连接池获取超时, available: {}", connections.size());
            return nullptr;
        }
        if (stopEnable || connections.empty()) {
            return nullptr;
        }
        auto context = std::move(connections.front());

        connections.pop();
        return context;
    }

    void ReturnConnection(std::unique_ptr<sw::redis::Redis> context)
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        if (stopEnable) {
            return;
        }
        connections.push(std::move(context));
        condVar.notify_one();
    }

    void Close()
    {
        stopEnable = true;
        condVar.notify_all();
    }

    bool Empty()
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        return connections.empty();
    }

    size_t Size()
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        return connections.size();
    }

  private:
    std::unique_ptr<sw::redis::Redis> CreateConnection() const
    {
        // socket 上限防止已借出的 Redis 连接无限挂起；连接池获取仍服从当前请求
        // 更短的 deadline，两层限制分别约束排队时间和网络时间。
        sw::redis::ConnectionOptions options;
        options.password = password;
        options.connect_timeout = std::chrono::milliseconds(1000);
        options.socket_timeout = std::chrono::milliseconds(1000);

        // 所有读写都连接 Sentinel 当前认定的主节点。这样写入成功后，紧接着的读取
        // 不会因为副本还没同步而拿到旧数据。两个副本只在主节点不可用时接替它，
        // 平时不承担应用请求。
        return std::make_unique<sw::redis::Redis>(sentinel, masterName, sw::redis::Role::MASTER, options);
    }

    void keepConnectionHandle()
    {
        std::lock_guard<std::mutex> lock(poolMutex);

        std::size_t len = connections.size();
        for (int i = 0; i < len; i++) {
            if (stopEnable)
                return;
            auto con = std::move(connections.front());
            connections.pop();
            try {
                auto pong = con->ping();
                if (pong != "PONG")
                    throw sw::redis::Error("PING command is failed");

                connections.push(std::move(con));
            } catch (sw::redis::Error& e) {
                LOG_DEBUG(logger, "redis connection is error: {}, reconnect....", e.what());
                con.reset();
                try {
                    auto session = CreateConnection();
                    connections.push(std::move(session));
                } catch (sw::redis::Error& e) {
                    spdlog::warn("redis reconnection is error: {} | master: {}", e.what(), masterName);
                }
            }
        }
    }

  private:
    std::atomic<bool> stopEnable;
    std::shared_ptr<sw::redis::Sentinel> sentinel;
    std::string masterName;
    std::string password;
    std::queue<std::unique_ptr<sw::redis::Redis>> connections;
    std::mutex poolMutex;
    std::condition_variable condVar;
    std::thread keepThread;
};

class RedisDao : public Singleton<RedisDao>, std::enable_shared_from_this<RedisPool>
{
    friend class TestDb;

  public:
    using Ptr = std::shared_ptr<RedisDao>;

    RedisDao()
    {
        static bool inited = false;
        if (inited) {
            LOG_WARN(logger, "RedisDao is already inited");
            return;
        }
        auto conf = Configer::getNode("server");
        auto masterName = conf["redis"]["masterName"].as<std::string>();
        auto password = conf["redis"]["password"].as<std::string>();
        auto sentinelPassword = conf["redis"]["sentinelPassword"].as<std::string>();
        auto clientCount = conf["redis"]["clientCount"].as<int>();
        auto machine = conf["redis"]["machineId"].as<int>();

        sw::redis::SentinelOptions sentinelOptions;
        sentinelOptions.password = sentinelPassword;
        for (const auto& node : conf["redis"]["sentinels"]) {
            sentinelOptions.nodes.emplace_back(node["host"].as<std::string>(), node["port"].as<int>());
        }
        sentinelOptions.connect_timeout = std::chrono::milliseconds(1000);
        sentinelOptions.socket_timeout = std::chrono::milliseconds(1000);
        auto sentinel = std::make_shared<sw::redis::Sentinel>(sentinelOptions);

        redisPool.reset(new RedisPool(std::move(sentinel), masterName, password, clientCount));
        machineId = machine;

        if (!redisPool->Empty()) {
            LOG_INFO(logger,
                     "redis connection pool init success | master: {}, sentinelCount: "
                     "{}, clientCount: {}, machineId: {}",
                     masterName, sentinelOptions.nodes.size(), clientCount, machine);
        } else {
            LOG_WARN(logger,
                     "redis connection pool init failed | master: {}, sentinelCount: "
                     "{}, clientCount: {}, machineId: {}",
                     masterName, sentinelOptions.nodes.size(), clientCount, machine);
        }
        inited = true;
    }

    /*
    接口说明：【2025-5-2】
      对于int、long返回类型，返回-1表示存储时出错
      对于指针（shared_ptr）返回类型，错误时统一返回nullptr
      对于bool返回类型，false表示错误
      返回接口定义处见：defaultForType
    */
    template <typename Func>
    auto executeTemplate(Func&& processor) -> decltype(processor(std::declval<std::unique_ptr<sw::redis::Redis>&>()))
    {
        auto con = redisPool->GetConnection();
        if (con == nullptr) {
            LOG_INFO(logger, "Redis connection pool is empty!");
            return defaultForType<decltype(processor(con))>();
        }
        Defer defer([&con, this]() { redisPool->ReturnConnection(std::move(con)); });

        try {
            return processor(con);
        } catch (sw::redis::Error& e) {
            LOG_TRACE(logger, "MySQL error: {}", e.what());
            return defaultForType<decltype(processor(con))>();
        }
    }
    template <typename T> static auto defaultForType()
    {
        if constexpr (std::is_same_v<T, bool>) {
            return false;
        } else if constexpr (std::is_integral_v<T>) {
            return -1;
        } else {
            return T{};
        }
    }

    void Close() { return redisPool->Close(); }

    const std::string PrefixSessionLease = "im:session:";
    const std::string PrefixSessionGeneration = "im:sessionGeneration:";
    const std::string PrefixUserSessions = "im:userSessions:";

    int64_t bindSessionLease(long uid, const std::string& deviceId, const std::string& gatewayId,
                             const std::string& instanceId, const std::string& connectionId, long ttlSeconds)
    {
        if (uid <= 0 || deviceId.empty() || gatewayId.empty() || instanceId.empty() || connectionId.empty() ||
            ttlSeconds <= 0)
            return -1;
        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) {
            static const std::string script = R"(
local generation = redis.call('INCR', KEYS[1])
local value = cjson.encode({deviceId=ARGV[1], gatewayId=ARGV[2], instanceId=ARGV[3], connectionId=ARGV[4], generation=generation})
redis.call('SETEX', KEYS[2], ARGV[5], value)
redis.call('SADD', KEYS[3], ARGV[1])
redis.call('EXPIRE', KEYS[3], ARGV[6])
return generation
)";
            const std::string generationKey = PrefixSessionGeneration + std::to_string(uid) + ":" + deviceId;
            const std::string leaseKey = PrefixSessionLease + std::to_string(uid) + ":" + deviceId;
            const std::string indexKey = PrefixUserSessions + std::to_string(uid);
            const std::string ttl = std::to_string(ttlSeconds);
            const std::string indexTtl = std::to_string(ttlSeconds * 2);
            return redis->eval<long long>(script, {generationKey, leaseKey, indexKey},
                                          {deviceId, gatewayId, instanceId, connectionId, ttl, indexTtl});
        });
    }

    SessionLease getSessionLease(long uid, const std::string& deviceId)
    {
        if (uid <= 0 || deviceId.empty())
            return {};
        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) {
            auto source = redis->get(PrefixSessionLease + std::to_string(uid) + ":" + deviceId);
            if (!source)
                return SessionLease{};
            Json::Value value;
            Json::Reader reader;
            if (!reader.parse(*source, value))
                return SessionLease{};
            SessionLease lease;
            lease.deviceId = value["deviceId"].asString();
            lease.gatewayId = value["gatewayId"].asString();
            lease.instanceId = value["instanceId"].asString();
            lease.connectionId = value["connectionId"].asString();
            lease.generation = value["generation"].asInt64();
            return lease;
        });
    }

    std::vector<SessionLease> getSessionLeases(long uid)
    {
        if (uid <= 0)
            return {};
        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) -> std::vector<SessionLease> {
            const std::string indexKey = PrefixUserSessions + std::to_string(uid);
            std::vector<std::string> deviceIds;
            redis->smembers(indexKey, std::back_inserter(deviceIds));
            std::vector<SessionLease> leases;
            for (const auto& deviceId : deviceIds) {
                auto source = redis->get(PrefixSessionLease + std::to_string(uid) + ":" + deviceId);
                if (!source) {
                    redis->srem(indexKey, deviceId);
                    continue;
                }
                Json::Value value;
                Json::Reader reader;
                if (!reader.parse(*source, value)) {
                    redis->srem(indexKey, deviceId);
                    continue;
                }
                SessionLease lease;
                lease.deviceId = value["deviceId"].asString();
                lease.gatewayId = value["gatewayId"].asString();
                lease.instanceId = value["instanceId"].asString();
                lease.connectionId = value["connectionId"].asString();
                lease.generation = value["generation"].asInt64();
                if (!lease.empty())
                    leases.push_back(std::move(lease));
            }
            return leases;
        });
    }

    bool refreshSessionLease(long uid, const std::string& deviceId, const SessionLease& lease, long ttlSeconds)
    {
        if (uid <= 0 || deviceId.empty() || lease.empty() || ttlSeconds <= 0)
            return false;
        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) {
            static const std::string script = R"(
local value = redis.call('GET', KEYS[1])
if not value then return 0 end
local lease = cjson.decode(value)
if lease.deviceId ~= ARGV[1] or lease.gatewayId ~= ARGV[2] or lease.instanceId ~= ARGV[3] or lease.connectionId ~= ARGV[4] or tostring(lease.generation) ~= ARGV[5] then return 0 end
redis.call('EXPIRE', KEYS[1], ARGV[6])
redis.call('SADD', KEYS[2], ARGV[1])
redis.call('EXPIRE', KEYS[2], ARGV[7])
return 1
)";
            const std::string key = PrefixSessionLease + std::to_string(uid) + ":" + deviceId;
            const std::string indexKey = PrefixUserSessions + std::to_string(uid);
            const std::string generation = std::to_string(lease.generation);
            const std::string ttl = std::to_string(ttlSeconds);
            const std::string indexTtl = std::to_string(ttlSeconds * 2);
            return redis->eval<long long>(script, {key, indexKey},
                                          {deviceId, lease.gatewayId, lease.instanceId, lease.connectionId, generation,
                                           ttl, indexTtl}) == 1;
        });
    }

    bool clearSessionLease(long uid, const std::string& deviceId, const SessionLease& lease)
    {
        if (uid <= 0 || deviceId.empty() || lease.empty())
            return false;
        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) {
            static const std::string script = R"(
local value = redis.call('GET', KEYS[1])
if not value then redis.call('SREM', KEYS[2], ARGV[1]) return 1 end
local lease = cjson.decode(value)
if lease.deviceId ~= ARGV[1] or lease.gatewayId ~= ARGV[2] or lease.instanceId ~= ARGV[3] or lease.connectionId ~= ARGV[4] or tostring(lease.generation) ~= ARGV[5] then return 0 end
redis.call('DEL', KEYS[1])
redis.call('SREM', KEYS[2], ARGV[1])
return 1
)";
            const std::string key = PrefixSessionLease + std::to_string(uid) + ":" + deviceId;
            const std::string indexKey = PrefixUserSessions + std::to_string(uid);
            const std::string generation = std::to_string(lease.generation);
            return redis->eval<long long>(
                       script, {key, indexKey},
                       {deviceId, lease.gatewayId, lease.instanceId, lease.connectionId, generation}) == 1;
        });
    }

    // Gate -> Chat 的短期身份交接凭证；token 值不得写入日志。
    const std::string PrefixChatAuthToken = "im:chatAuth:";
    bool setChatAuthToken(long uid, const std::string& token, long ttlSeconds)
    {
        if (uid <= 0 || token.empty() || ttlSeconds <= 0)
            return false;
        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) {
            redis->setex(PrefixChatAuthToken + std::to_string(uid), ttlSeconds, token);
            return true;
        });
    }

    bool validateChatAuthToken(long uid, const std::string& token)
    {
        if (uid <= 0 || token.empty())
            return false;
        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) {
            auto stored = redis->get(PrefixChatAuthToken + std::to_string(uid));
            return stored.has_value() && stored.value() == token;
        });
    }

    const std::string __prefixUid = "im:userId";
    int64_t generateUserId() { return generateId(__prefixUid); }

    const std::string __prefixMsgId = "im:msgId";
    int64_t generateMsgId() { return generateId(__prefixMsgId); }

    const std::string __prefixConversationId = "im:conversationId";
    int64_t generateConversationId() { return generateId(__prefixConversationId); }

    const std::string __prefixFriendshipId = "im:friendshipId";
    int64_t generateFriendshipId() { return generateId(__prefixFriendshipId); }

    const std::string __prefixGroupId = "im:groupId";
    int64_t generateGroupId() { return generateId(__prefixGroupId); }

    int issueVerificationCode(const std::string& email, const std::string& code, long ttlSeconds, long cooldownSeconds)
    {
        if (email.empty() || code.empty() || ttlSeconds <= 0 || cooldownSeconds <= 0)
            return -1;

        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) {
            static const std::string script = R"(
if redis.call('EXISTS', KEYS[2]) == 1 then return 0 end
redis.call('SETEX', KEYS[1], ARGV[2], ARGV[1])
redis.call('SETEX', KEYS[2], ARGV[3], '1')
redis.call('DEL', KEYS[3])
return 1
)";
            const std::string prefix = "im:verify:{" + email + "}:";
            return static_cast<int>(
                redis->eval<long long>(script, {prefix + "code", prefix + "cooldown", prefix + "attempts"},
                                       {code, std::to_string(ttlSeconds), std::to_string(cooldownSeconds)}));
        });
    }

    bool consumeVerificationCode(const std::string& email, const std::string& code, int maxAttempts)
    {
        if (email.empty() || code.empty() || maxAttempts <= 0)
            return false;

        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) {
            static const std::string script = R"(
local current = redis.call('GET', KEYS[1])
if not current then return 0 end
if current == ARGV[1] then
  redis.call('DEL', KEYS[1], KEYS[2], KEYS[3])
  return 1
end
local attempts = redis.call('INCR', KEYS[2])
local ttl = redis.call('TTL', KEYS[1])
if attempts == 1 and ttl > 0 then redis.call('EXPIRE', KEYS[2], ttl) end
if attempts >= tonumber(ARGV[2]) then
  redis.call('DEL', KEYS[1], KEYS[2])
end
return 0
)";
            const std::string prefix = "im:verify:{" + email + "}:";
            return redis->eval<long long>(script, {prefix + "code", prefix + "attempts", prefix + "cooldown"},
                                          {code, std::to_string(maxAttempts)}) == 1;
        });
    }

    std::string getVerificationCode(const std::string& email)
    {
        if (email.empty())
            return {};
        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) {
            const std::string key = "im:verify:{" + email + "}:code";
            auto code = redis->get(key);
            return code.value_or("");
        });
    }

    bool clearVerificationCode(const std::string& email, const std::string& expectedCode)
    {
        if (email.empty() || expectedCode.empty())
            return false;
        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) {
            static const std::string script = R"(
if redis.call('GET', KEYS[1]) ~= ARGV[1] then return 0 end
redis.call('DEL', KEYS[1], KEYS[2], KEYS[3])
return 1
)";
            const std::string prefix = "im:verify:{" + email + "}:";
            return redis->eval<long long>(script, {prefix + "code", prefix + "attempts", prefix + "cooldown"},
                                          {expectedCode}) == 1;
        });
    }

  private:
    int64_t generateId(const std::string& key)
    {
        return executeTemplate([&](std::unique_ptr<sw::redis::Redis>& redis) -> int64_t {
            // 1. 获取当前毫秒时间戳（41位）
            auto now = std::chrono::system_clock::now();
            auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() - epoch;

            // 2. 获取序列号（12位，每毫秒4096个）
            auto seq = redis->incr(key + ":" + std::to_string(ts));
            if (seq > 4095) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return generateId(key); // 递归直到不冲突
            }

            // 3. 组合ID：时间戳(41) + 机器ID(10) + 序列号(12)
            return (ts << 22) | (machineId << 12) | seq;
        });
    }

  private:
    RedisPool::Ptr redisPool;
    uint16_t machineId;                         // 集群中每个节点的唯一ID (0-1023)
    static const int64_t epoch = 1672531200000; // 2023-01-01 00:00:00
};

}; // namespace wimi::db
