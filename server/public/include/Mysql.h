#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <exception>
#include <functional>
#include <mysql-cppconn/mysqlx/devapi/common.h>
#include <mysql-cppconn/mysqlx/devapi/result.h>
#include <mysql-cppconn/mysqlx/xdevapi.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "Configer.h"
#include "Const.h"
#include "DbGlobal.h"
#include "Logger.h"
#include "Metrics.h"
#include "RequestContext.h"

namespace wimi::db
{

struct MessageAcceptResult {
    int error{ErrorCodes::MysqlFailed};
    std::string diagnostic;
    bool duplicate{false};
    int64_t messageId{0};
    int64_t conversationId{0};
    int64_t conversationSeq{0};
};

struct GroupMessageAcceptResult : MessageAcceptResult {
    int64_t groupId{0};
    std::vector<int64_t> recipientUids;
};

struct ConversationMessageRecord {
    int64_t messageId{0};
    int64_t senderId{0};
    int64_t receiverId{0};
    int64_t conversationId{0};
    int64_t conversationSeq{0};
    std::string clientMessageId;
    int type{0};
    std::string content;
    int status{0};
    std::string sendDateTime;
    std::string readDateTime;
    struct FileAttachment {
        std::string fileId;
        std::string fileName;
        std::string fileType;
        int64_t fileSize{0};
        std::string sha256;
        bool expired{false};
    };
    std::optional<FileAttachment> file;
};

struct FileObjectRecord {
    std::string fileId;
    long ownerUid{0};
    std::string fileName;
    std::string fileType;
    int64_t fileSize{0};
    std::string sha256;
    bool expired{false};
};

struct ConversationSyncResult {
    int error{ErrorCodes::MysqlFailed};
    int conversationType{0};
    int64_t latestSeq{0};
    int64_t nextSeq{0};
    bool hasMore{false};
    std::vector<ConversationMessageRecord> messages;
};

struct ConversationAckResult {
    int updated{-1};
    int64_t senderUid{0};
    bool directConversation{false};
    bool readStateChanged{false};
};

struct SqlConnection {
    SqlConnection(mysqlx::Session* connection, int64_t lastTime) : session(connection), leaseTime(lastTime) {}
    ~SqlConnection()
    {
        if (session) {
            session->close();
            session.reset();
        }
    }

    std::unique_ptr<mysqlx::Session> session;
    int64_t leaseTime;
};

class MysqlPool
{
  public:
    using Ptr = std::shared_ptr<MysqlPool>;

    MysqlPool(const std::string& host, unsigned short port, const std::string& user, const std::string& password,
              const std::string& schema, std::size_t maxSize = 2)
        : host(host), port(port), user(user), password(password), schema(schema)
    {
        for (size_t i = 0; i < maxSize; ++i) {
            try {
                mysqlx::Session* sqlSession(CreateSession());

                auto currentTime = std::chrono::system_clock::now().time_since_epoch();
                int64_t leaseTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime).count();
                sqlSession->sql("SELECT 1").execute();

                pool.push(std::make_unique<SqlConnection>(sqlSession, leaseTime));

            } catch (mysqlx::Error& e) {
                LOG_WARN(wimi::dbLogger, "Mysql-client init(number-{}) error: {}", i, e.what());
                continue;
            }
        }

        if (pool.empty()) {
            LOG_DEBUG(wimi::dbLogger, "Mysql-client pool init error! | poolSize: {}", maxSize);
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

  public:
    std::unique_ptr<SqlConnection> GetConnection()
    {
        return GetConnectionUntil(RequestContextScope::CurrentDeadlineOr(std::chrono::milliseconds(1000)));
    }

    std::unique_ptr<SqlConnection> GetConnectionUntil(RequestContext::Deadline deadline)
    {
        std::unique_lock<std::mutex> lock(sqlMutex);
        bool ready = condVar.wait_until(lock, deadline, [this] {
            if (stopEnable)
                return true;
            return !pool.empty();
        });
        if (!ready) {
            Metrics::Increment(Metric::MysqlAcquireTimeout);
            LOG_WARN(wimi::dbLogger, "MySQL连接池获取超时, available: {}", pool.size());
            return nullptr;
        }
        if (stopEnable || pool.empty()) {
            return nullptr;
        }
        std::unique_ptr<SqlConnection> con(std::move(pool.front()));
        pool.pop();
        return con;
    }

    std::unique_ptr<SqlConnection> ReturnConnection(std::unique_ptr<SqlConnection> con)
    {
        std::unique_lock<std::mutex> lock(sqlMutex);
        if (stopEnable)
            return con;

        pool.push(std::move(con));
        condVar.notify_one();
        return nullptr;
    }

    bool Empty()
    {
        std::lock_guard<std::mutex> lock(sqlMutex);
        return pool.empty();
    }

    void Close()
    {
        // 每个等待锁的调用线程都会被唤醒，
        // 并且每个锁相关函数都在锁跳出的的下一步检查stopEnable标志，如果标志为true则跳出
        stopEnable = true;
        condVar.notify_all();
    }
    std::size_t Size()
    {
        std::lock_guard<std::mutex> lock(sqlMutex);
        return pool.size();
    }

    ~MysqlPool()
    {
        Close();
        std::unique_lock<std::mutex> lock(sqlMutex);
        while (!pool.empty()) {
            pool.pop();
            LOG_INFO(wimi::dbLogger, "Mysql-client pool destroy! | poolSize: {}", pool.size());
        }
        if (keepThread.joinable()) {
            keepThread.join();
        }
    }

  private:
    mysqlx::Session* CreateSession() const
    {
        // X DevAPI 无法为已建连接按单请求动态修改 socket timeout；固定上限负责
        // 截断半开连接，请求级 deadline 仍由连接池等待和上层幂等语义负责。
        mysqlx::SessionSettings settings(mysqlx::SessionOption::HOST, host, mysqlx::SessionOption::PORT, port,
                                         mysqlx::SessionOption::USER, user, mysqlx::SessionOption::PWD, password,
                                         mysqlx::SessionOption::DB, schema, mysqlx::SessionOption::CONNECT_TIMEOUT,
                                         std::chrono::milliseconds(1000), mysqlx::SessionOption::READ_TIMEOUT,
                                         std::chrono::milliseconds(1000), mysqlx::SessionOption::WRITE_TIMEOUT,
                                         std::chrono::milliseconds(1000));
        return new mysqlx::Session(settings);
    }

    void keepConnectionHandle()
    {
        std::lock_guard<std::mutex> lock(sqlMutex);
        int poolsize = pool.size();
        // 获取当前时间戳
        auto currentTime = std::chrono::system_clock::now().time_since_epoch();
        // 将时间戳转换为秒
        long long leaseTime = std::chrono::duration_cast<std::chrono::seconds>(currentTime).count();

        for (int i = 0; i < poolsize; i++) {
            // 一旦Close后，就可跳出，无需继续处理
            if (stopEnable)
                return;

            auto con = std::move(pool.front());
            pool.pop();

            if (leaseTime - con->leaseTime < 5) {
                pool.push(std::move(con));
                continue;
            }

            try {
                mysqlx::SqlResult result = con->session->sql("SELECT 1").execute();
                con->leaseTime = leaseTime;
                pool.push(std::move(con));
            } catch (mysqlx::Error& e) {
                LOG_INFO(wimi::dbLogger, "Error keeping connection alive: {}, reconnect now...", e.what());
                try {
                    mysqlx::Session* newSession(CreateSession());
                    con->leaseTime = leaseTime;
                    con.reset();
                    pool.push(std::make_unique<SqlConnection>(newSession, leaseTime));
                } catch (mysqlx::Error& e) {
                    LOG_INFO(wimi::dbLogger, "Error reconnecting: {}", e.what());
                    return;
                }
            }
        }
    }

  private:
    std::string host;
    unsigned short port;
    std::string user;
    std::string password;
    std::string schema;

    std::queue<std::unique_ptr<SqlConnection>> pool{};
    std::mutex sqlMutex{};
    std::condition_variable condVar{};
    std::atomic<bool> stopEnable{};
    std::thread keepThread{};
}; // MysqlPool

class MysqlDao : public Singleton<MysqlDao>, public std::enable_shared_from_this<MysqlDao>
{
  private:
    MysqlPool::Ptr mysqlPool;
    friend class TestDb;

  public:
    using Ptr = std::shared_ptr<MysqlDao>;
    MysqlDao()
    {
        try {
            auto conf = Configer::getNode("server");

            auto host = conf["mysql"]["host"].as<std::string>();
            auto port = conf["mysql"]["port"].as<unsigned short>();
            auto user = conf["mysql"]["user"].as<std::string>();
            auto password = conf["mysql"]["password"].as<std::string>();
            auto schema = conf["mysql"]["schema"].as<std::string>();
            auto clientCount = conf["mysql"]["clientCount"].as<int>();
            mysqlPool.reset(new MysqlPool(host, port, user, password, schema, clientCount));

            if (!mysqlPool->Empty()) {
                LOG_INFO(wimi::dbLogger,
                         "Mysql-Client pool init success! | host: {}, port: {}, "
                         "user: {}, schema: {}, poolSize: {}",
                         host, port, user, schema, mysqlPool->Size());
            } else {
                LOG_WARN(wimi::dbLogger,
                         "Mysql-Client pool init failed! | host: {}, port: {}, "
                         "user: {}, schema: {}, poolSize: {}",
                         host, port, user, schema, mysqlPool->Size());
            }
        } catch (std::exception& e) {
            LOG_ERROR(wimi::dbLogger, "MysqlDao init error: {}", e.what());
        }
    }
    ~MysqlDao() {}

    /*
    接口说明：【2025-5-2】
      对于int、long返回类型，返回-1表示存储时出错；对于查询，返回1表示已存在该数据
      对于指针（shared_ptr）返回类型，错误时统一返回nullptr
      对于bool返回类型，false表示错误
      返回接口定义处见：defaultForType
    注意：为避免死锁，接口函数之间不得互相调用，因每个调用都会获取连接池，若池中没有可用连接，则会一直等待
    */
    template <typename Func>
    auto executeTemplate(Func&& processor) -> decltype(processor(std::declval<std::unique_ptr<mysqlx::Session>&>()))
    {
        auto con = mysqlPool->GetConnection();
        if (con == nullptr) {
            LOG_INFO(dbLogger, "Mysql connection pool is empty!");
            return defaultForType<decltype(processor(con->session))>();
        }
        Defer defer([&con, this]() { mysqlPool->ReturnConnection(std::move(con)); });

        try {
            return processor(con->session); // 完全由processor处理SQL执行
        } catch (mysqlx::Error& e) {
            LOG_ERROR(dbLogger, "MySQL error: {}", e.what());
            HandleError(e);
            return defaultForType<decltype(processor(con->session))>();
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
    void HandleError(mysqlx::Error& e)
    {
        // 暂无处理
        return;
    }

    void Close() { mysqlPool->Close(); }

    User::Ptr getUser(const std::string& username)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> User::Ptr {
            auto result = session->sql("SELECT * FROM users WHERE username = ?").bind(username).execute();
            if (!(result.count() > 0)) {
                LOG_DEBUG(dbLogger, "Mysql指令调用的结果集为空");
                return nullptr;
            }

            auto row = result.fetchOne();

            size_t id = row[0].get<size_t>();
            size_t uid = row[1].get<size_t>();
            std::string username = row[2].get<std::string>();
            std::string password = row[3].get<std::string>();
            std::string email = row[4].get<std::string>();
            std::string createTime = row[5].get<std::string>();
            return std::make_shared<User>(std::move(id), std::move(uid), std::move(username), std::move(password),
                                          std::move(email), createTime);
        });
    }

    UserInfo::Ptr getUserInfo(long uid)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> UserInfo::Ptr {
            auto result = session->sql("SELECT * FROM userInfo WHERE uid = ?").bind(uid).execute();
            if (!(result.count() > 0)) {
                LOG_DEBUG(dbLogger, "Mysql指令调用的结果集为空");
                return nullptr;
            }
            auto row = result.fetchOne();
            return std::make_shared<UserInfo>(uid,
                                              row[1].get<std::string>(), // name
                                              row[2].get<int>(),         // age
                                              row[3].get<std::string>(), // sex
                                              row[4].get<std::string>()  // headImageURL
            );
        });
    }

    bool hasUserInfo(long uid)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> bool {
            auto result = session->sql("SELECT 1 FROM userInfo WHERE uid = ?").bind(uid).execute();
            const auto row = result.fetchOne();
            return static_cast<bool>(row);
        });
    }

    int registerDevice(long uid, const std::string& deviceId, const std::string& platform = {},
                       const std::string& deviceName = {})
    {
        if (uid <= 0 || deviceId.empty())
            return 0;
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            session
                ->sql(
                    R"(INSERT INTO devices (deviceId, uid, platform, deviceName) VALUES (?, ?, ?, ?) ON DUPLICATE KEY UPDATE deviceId = VALUES(deviceId))")
                .bind(deviceId)
                .bind(uid)
                .bind(platform)
                .bind(deviceName)
                .execute();
            auto ownerResult = session->sql(R"(SELECT uid FROM devices WHERE deviceId = ?)").bind(deviceId).execute();
            auto owner = ownerResult.fetchOne();
            return owner && owner[0].get<int64_t>() == uid ? 1 : 0;
        });
    }

    Friend::FriendGroup getFriendList(long uid)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> Friend::FriendGroup {
            auto result =
                session
                    ->sql(
                        R"(SELECT f.friendshipId, c.conversationId, ?, CASE WHEN f.uidA = ? THEN f.uidB ELSE f.uidA END, f.createTime FROM friends f INNER JOIN conversations c ON c.type = 1 AND c.businessId = f.friendshipId WHERE f.uidA = ? OR f.uidB = ?)")
                    .bind(uid)
                    .bind(uid)
                    .bind(uid)
                    .bind(uid)
                    .execute();

            if (!(result.count() > 0)) {
                LOG_DEBUG(dbLogger, "Mysql指令调用的结果集为空");
                return nullptr;
            }

            auto friendGroup = std::make_shared<std::vector<Friend::Ptr>>();
            for (const auto& row : result.fetchAll()) {
                friendGroup->emplace_back(new Friend(row[0].get<size_t>(),     // friendshipId
                                                     row[1].get<size_t>(),     // conversationId
                                                     row[2].get<size_t>(),     // requested uid
                                                     row[3].get<size_t>(),     // peer uid
                                                     row[4].get<std::string>() // createTime
                                                     ));
            }
            return friendGroup;
        });
    }

    int userModifyPassword(User::Ptr user)
    {
        return executeTemplate([user](std::unique_ptr<mysqlx::Session>& session) -> int {
            // 验证用户存在
            auto checkResult = session->sql("SELECT * FROM users WHERE email = ? AND uid = ?")
                                   .bind(user->email)
                                   .bind(user->uid)
                                   .execute();

            if (!(checkResult.count() > 0)) {
                LOG_DEBUG(dbLogger, "Mysql指令调用的结果集为空");
                return 1; // 用户不存在
            }

            // 更新密码
            auto updateResult = session->sql("UPDATE users SET password = ? WHERE email = ? AND uid = ?")
                                    .bind(user->password)
                                    .bind(user->email)
                                    .bind(user->uid)
                                    .execute();

            return 0; // 成功
        });
    }

    int userRegister(User::Ptr user)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            // 检查用户是否已存在
            auto result = session->sql("SELECT COUNT(*) FROM users WHERE email = ? OR uid = ?")
                              .bind(user->email)
                              .bind(user->uid)
                              .execute();

            auto row = result.fetchOne();
            if (row && row[0].get<int64_t>() > 0) {
                LOG_INFO(dbLogger, "User already exists");
                return 1;
            }

            // 注册新用户
            auto createTime = getCurrentDateTime();
            result = session->sql("INSERT INTO users VALUES (NULL,?,?,?,?,?)")
                         .bind(user->uid)
                         .bind(user->username)
                         .bind(user->password)
                         .bind(user->email)
                         .bind(createTime)
                         .execute();

            return 0; // 注册成功
        });
    }

    int insertUserInfo(UserInfo::Ptr userInfo)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f = R"(INSERT INTO userInfo VALUES (?,?,?,?,?))";
            auto result = session->sql(f)
                              .bind(userInfo->uid)
                              .bind(userInfo->name)
                              .bind(userInfo->age)
                              .bind(userInfo->sex)
                              .bind(userInfo->headImageURL)
                              .execute();

            return 0;
        });
    }

    int insertFriendApply(FriendApply::Ptr friendData)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f = R"(INSERT INTO friendApplys VALUES (?, ?, ?, ?, ?))";
            // 存两份，便于两边查找
            auto result = session->sql(f)
                              .bind(friendData->from)
                              .bind(friendData->to)
                              .bind(friendData->content)
                              .bind(friendData->status)
                              .bind(friendData->createTime)
                              .execute();
            result = session->sql(f)
                         .bind(friendData->to)
                         .bind(friendData->from)
                         .bind(friendData->content)
                         .bind(friendData->status)
                         .bind(friendData->createTime)
                         .execute();
            return 0;
        });
    }

    int updateFriendApplyStatus(FriendApply::Ptr friendData)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            // update: status, content, createTime
            std::string f =
                R"(UPDATE friendApplys SET status = ?, content = ?, createTime = ? WHERE fromUid = ? AND toUid = ?)";
            auto result = session->sql(f)
                              .bind(friendData->status)
                              .bind(friendData->content)
                              .bind(friendData->createTime)
                              .bind(friendData->from)
                              .bind(friendData->to)
                              .execute();

            result = session->sql(f)
                         .bind(friendData->status)
                         .bind(friendData->content)
                         .bind(friendData->createTime)
                         .bind(friendData->to)
                         .bind(friendData->from)
                         .execute();
            return 0;
        });
    }
    int hasFriend(long uidA, long uidB)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            const auto first = std::min(uidA, uidB);
            const auto second = std::max(uidA, uidB);
            auto result =
                session->sql(R"(SELECT 1 FROM friends WHERE uidA = ? AND uidB = ?)").bind(first).bind(second).execute();
            return (result.fetchOne() != 0);
        });
    }
    int insertFriend(Friend::Ptr friendData)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string dateTime = getCurrentDateTime();
            const auto uidA = std::min(friendData->uidA, friendData->uidB);
            const auto uidB = std::max(friendData->uidA, friendData->uidB);
            session->startTransaction();
            try {
                session->sql(R"(INSERT INTO friends (friendshipId, uidA, uidB, createTime) VALUES (?, ?, ?, ?))")
                    .bind(friendData->friendshipId)
                    .bind(uidA)
                    .bind(uidB)
                    .bind(dateTime)
                    .execute();
                session
                    ->sql(
                        R"(INSERT INTO conversations (conversationId, type, businessId, latestSeq, createTime) VALUES (?, 1, ?, 0, ?))")
                    .bind(friendData->conversationId)
                    .bind(friendData->friendshipId)
                    .bind(dateTime)
                    .execute();
                session
                    ->sql(
                        R"(INSERT INTO conversationUserStates (conversationId, uid, joinedSeq) VALUES (?, ?, 1), (?, ?, 1))")
                    .bind(friendData->conversationId)
                    .bind(uidA)
                    .bind(friendData->conversationId)
                    .bind(uidB)
                    .execute();
                session->commit();
                return 0;
            } catch (...) {
                try {
                    session->rollback();
                } catch (...) {
                }
                throw;
            }
        });
    }

    int insertMessage(Message::Ptr message)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f =
                R"(INSERT INTO messages (messageId, senderId, receiverId, conversationId, conversationSeq, type, content, status, sendDateTime, readDateTime) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?))";
            auto result = session->sql(f)
                              .bind(message->messageId)
                              .bind(message->from)
                              .bind(message->to)
                              .bind(message->conversationId)
                              .bind(message->conversationSeq)
                              .bind(message->type)
                              .bind(message->content)
                              .bind(message->status)
                              .bind(message->sendDateTime)
                              .bind(message->readDateTime)
                              .execute();

            return 0;
        });
    }

    MessageAcceptResult acceptDirectText(long senderId, long receiverId, const std::string& clientMessageId,
                                         const std::string& content, const std::string& sendDateTime,
                                         int messageType = Message::Type::TEXT)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> MessageAcceptResult {
            MessageAcceptResult accepted;
            session->startTransaction();
            try {
                auto friendResult =
                    session
                        ->sql(
                            R"(SELECT c.conversationId FROM friends f INNER JOIN conversations c ON c.type = 1 AND c.businessId = f.friendshipId WHERE f.uidA = ? AND f.uidB = ?)")
                        .bind(std::min(senderId, receiverId))
                        .bind(std::max(senderId, receiverId))
                        .execute();
                auto friendRow = friendResult.fetchOne();
                if (!friendRow) {
                    session->rollback();
                    accepted.error = ErrorCodes::UserNotFriend;
                    return accepted;
                }

                accepted.conversationId = friendRow[0].get<int64_t>();
                session
                    ->sql(
                        R"(INSERT IGNORE INTO conversationUserStates (conversationId, uid, joinedSeq) VALUES (?, ?, 1), (?, ?, 1))")
                    .bind(accepted.conversationId)
                    .bind(senderId)
                    .bind(accepted.conversationId)
                    .bind(receiverId)
                    .execute();

                auto sequenceResult =
                    session->sql(R"(SELECT latestSeq FROM conversations WHERE conversationId = ? FOR UPDATE)")
                        .bind(accepted.conversationId)
                        .execute();
                auto sequenceRow = sequenceResult.fetchOne();
                if (!sequenceRow) {
                    session->rollback();
                    accepted.error = ErrorCodes::MysqlFailed;
                    accepted.diagnostic = "conversation row missing after insert";
                    return accepted;
                }

                auto duplicateResult =
                    session
                        ->sql(
                            R"(SELECT m.messageId,
                                      m.conversationId,
                                      m.conversationSeq,
                                      m.receiverId,
                                      m.type,
                                      m.content
                               FROM messages m
                               WHERE m.senderId = ? AND m.clientMessageId = ?)")
                        .bind(senderId)
                        .bind(clientMessageId)
                        .execute();
                auto duplicateRow = duplicateResult.fetchOne();
                if (duplicateRow) {
                    const bool sameCommand = duplicateRow[1].get<int64_t>() == accepted.conversationId &&
                                             duplicateRow[3].get<int64_t>() == receiverId &&
                                             duplicateRow[4].get<int>() == messageType &&
                                             duplicateRow[5].get<std::string>() == content;
                    if (!sameCommand) {
                        session->rollback();
                        accepted.error = ErrorCodes::IdempotencyConflict;
                        return accepted;
                    }
                    accepted.messageId = duplicateRow[0].get<int64_t>();
                    accepted.conversationSeq = duplicateRow[2].get<int64_t>();
                    accepted.duplicate = true;
                    accepted.error = ErrorCodes::Success;
                    session->commit();
                    return accepted;
                }

                accepted.conversationSeq = sequenceRow[0].get<int64_t>() + 1;
                session->sql(R"(UPDATE conversations SET latestSeq = ? WHERE conversationId = ?)")
                    .bind(accepted.conversationSeq)
                    .bind(accepted.conversationId)
                    .execute();

                const std::string canonicalCommand = std::to_string(accepted.conversationId) + ":" +
                                                     std::to_string(messageType) + ":" +
                                                     content;
                const std::string commandHash = std::to_string(std::hash<std::string>{}(canonicalCommand));
                auto insertResult =
                    session
                        ->sql(
                            R"(INSERT INTO messages (senderId, receiverId, conversationId, conversationSeq, clientMessageId, commandHash, type, content, status, sendDateTime, readDateTime) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ''))")
                        .bind(senderId)
                        .bind(receiverId)
                        .bind(accepted.conversationId)
                        .bind(accepted.conversationSeq)
                        .bind(clientMessageId)
                        .bind(commandHash)
                        .bind(messageType)
                        .bind(content)
                        .bind(static_cast<int>(Message::Status::WAIT))
                        .bind(sendDateTime)
                        .execute();
                accepted.messageId = static_cast<int64_t>(insertResult.getAutoIncrementValue());
                session->commit();
                accepted.error = ErrorCodes::Success;
                return accepted;
            } catch (const std::exception& error) {
                try {
                    session->rollback();
                } catch (...) {
                }
                accepted.error = ErrorCodes::MysqlFailed;
                accepted.diagnostic = error.what();
                return accepted;
            }
        });
    }

    int upsertReadyFile(const FileObjectRecord& file, const std::string& createdTime, const std::string& expiresTime)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            auto existing = session
                                ->sql(R"(SELECT ownerUid FROM fileObjects WHERE fileId = ? FOR UPDATE)")
                                .bind(file.fileId)
                                .execute()
                                .fetchOne();
            if (existing && existing[0].get<long>() != file.ownerUid) {
                return 1;
            }

            session
                ->sql(
                    R"(INSERT INTO fileObjects
                           (fileId, ownerUid, fileName, fileType, fileSize, sha256, state, createdTime, expiresTime)
                       VALUES (?, ?, ?, ?, ?, ?, 1, ?, ?)
                       ON DUPLICATE KEY UPDATE
                           fileName = VALUES(fileName),
                           fileType = VALUES(fileType),
                           fileSize = VALUES(fileSize),
                           sha256 = VALUES(sha256),
                           state = 1,
                           expiresTime = VALUES(expiresTime))")
                .bind(file.fileId)
                .bind(file.ownerUid)
                .bind(file.fileName)
                .bind(file.fileType)
                .bind(file.fileSize)
                .bind(mysqlx::bytes(reinterpret_cast<const mysqlx::byte*>(file.sha256.data()), file.sha256.size()))
                .bind(createdTime)
                .bind(expiresTime)
                .execute();
            return 0;
        });
    }

    int saveFileUploadProgress(long ownerUid, const std::string& fileId, int64_t nextOffset, bool complete,
                               const std::string& updateTime)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            session
                ->sql(
                    R"(INSERT INTO fileUploads (ownerUid, fileId, nextOffset, complete, updateTime)
                       VALUES (?, ?, ?, ?, ?)
                       ON DUPLICATE KEY UPDATE
                           nextOffset = VALUES(nextOffset),
                           complete = VALUES(complete),
                           updateTime = VALUES(updateTime))")
                .bind(ownerUid)
                .bind(fileId)
                .bind(nextOffset)
                .bind(complete ? 1 : 0)
                .bind(updateTime)
                .execute();
            return 0;
        });
    }

    std::optional<FileObjectRecord> findDownloadableFile(const std::string& fileId, long uid)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> std::optional<FileObjectRecord> {
            auto result =
                session
                    ->sql(
                        R"(SELECT f.fileId, f.ownerUid, f.fileName, f.fileType, f.fileSize, f.sha256, f.state
                           FROM fileObjects f
                           INNER JOIN messages m
                               ON m.content = f.fileId AND m.type = 5
                           INNER JOIN conversationUserStates s
                               ON s.conversationId = m.conversationId AND s.uid = ?
                           WHERE f.fileId = ? AND f.state = 1 AND f.expiresTime > ?
                           LIMIT 1)")
                    .bind(uid)
                    .bind(fileId)
                    .bind(getCurrentDateTime())
                    .execute();
            auto row = result.fetchOne();
            if (!row) {
                return std::nullopt;
            }

            FileObjectRecord file;
            file.fileId = row[0].get<std::string>();
            file.ownerUid = row[1].get<long>();
            file.fileName = row[2].get<std::string>();
            file.fileType = row[3].get<std::string>();
            file.fileSize = row[4].get<int64_t>();

            const auto sha256 = row[5].get<mysqlx::bytes>();
            file.sha256.assign(reinterpret_cast<const char*>(sha256.begin()), sha256.size());
            return file;
        });
    }

    std::optional<FileObjectRecord> findReadyFile(const std::string& fileId, long ownerUid)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> std::optional<FileObjectRecord> {
            auto result =
                session
                    ->sql(
                        R"(SELECT fileId, ownerUid, fileName, fileType, fileSize, sha256, state
                           FROM fileObjects
                           WHERE fileId = ? AND ownerUid = ? AND state = 1 AND expiresTime > ?)")
                    .bind(fileId)
                    .bind(ownerUid)
                    .bind(getCurrentDateTime())
                    .execute();
            auto row = result.fetchOne();
            if (!row) {
                return std::nullopt;
            }

            FileObjectRecord file;
            file.fileId = row[0].get<std::string>();
            file.ownerUid = row[1].get<long>();
            file.fileName = row[2].get<std::string>();
            file.fileType = row[3].get<std::string>();
            file.fileSize = row[4].get<int64_t>();

            const auto sha256 = row[5].get<mysqlx::bytes>();
            file.sha256.assign(reinterpret_cast<const char*>(sha256.begin()), sha256.size());
            return file;
        });
    }

    GroupMessageAcceptResult acceptGroupText(long senderId, long groupId, const std::string& clientMessageId,
                                             const std::string& content, const std::string& sendDateTime)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> GroupMessageAcceptResult {
            GroupMessageAcceptResult accepted;
            accepted.groupId = groupId;
            session->startTransaction();
            try {
                auto groupResult =
                    session
                        ->sql(
                            R"(SELECT c.conversationId, gm.speech FROM groupInfo gi INNER JOIN conversations c ON c.type = 2 AND c.businessId = gi.groupId INNER JOIN groupMembers gm ON gm.groupId = gi.groupId WHERE gi.groupId = ? AND gm.uid = ?)")
                        .bind(groupId)
                        .bind(senderId)
                        .execute();
                auto groupRow = groupResult.fetchOne();
                if (!groupRow) {
                    session->rollback();
                    accepted.error = ErrorCodes::MessageOwnershipInvalid;
                    return accepted;
                }
                if (groupRow[1].get<int>() != 0) {
                    session->rollback();
                    accepted.error = ErrorCodes::GroupNotifyFailed;
                    return accepted;
                }

                accepted.conversationId = groupRow[0].get<int64_t>();
                session
                    ->sql(
                        R"(INSERT INTO conversations (conversationId, type, businessId, latestSeq, createTime) VALUES (?, 2, ?, 0, ?) ON DUPLICATE KEY UPDATE conversationId = VALUES(conversationId))")
                    .bind(accepted.conversationId)
                    .bind(groupId)
                    .bind(sendDateTime)
                    .execute();
                session
                    ->sql(
                        R"(INSERT IGNORE INTO conversationUserStates (conversationId, uid, joinedSeq) SELECT ?, gm.uid, c.latestSeq + 1 FROM groupMembers gm INNER JOIN conversations c ON c.conversationId = ? WHERE gm.groupId = ?)")
                    .bind(accepted.conversationId)
                    .bind(accepted.conversationId)
                    .bind(groupId)
                    .execute();

                auto sequenceResult =
                    session->sql(R"(SELECT latestSeq FROM conversations WHERE conversationId = ? FOR UPDATE)")
                        .bind(accepted.conversationId)
                        .execute();
                auto sequenceRow = sequenceResult.fetchOne();
                if (!sequenceRow) {
                    session->rollback();
                    accepted.error = ErrorCodes::MysqlFailed;
                    accepted.diagnostic = "group conversation row missing after insert";
                    return accepted;
                }

                auto duplicateResult =
                    session
                        ->sql(
                            R"(SELECT messageId, conversationId, conversationSeq, receiverId, type, content FROM messages WHERE senderId = ? AND clientMessageId = ?)")
                        .bind(senderId)
                        .bind(clientMessageId)
                        .execute();
                auto duplicateRow = duplicateResult.fetchOne();
                if (duplicateRow) {
                    const bool sameCommand = duplicateRow[1].get<int64_t>() == accepted.conversationId &&
                                             duplicateRow[3].get<int64_t>() == groupId &&
                                             duplicateRow[4].get<int>() == Message::Type::TEXT &&
                                             duplicateRow[5].get<std::string>() == content;
                    if (!sameCommand) {
                        session->rollback();
                        accepted.error = ErrorCodes::IdempotencyConflict;
                        return accepted;
                    }
                    accepted.messageId = duplicateRow[0].get<int64_t>();
                    accepted.conversationSeq = duplicateRow[2].get<int64_t>();
                    accepted.duplicate = true;
                } else {
                    accepted.conversationSeq = sequenceRow[0].get<int64_t>() + 1;
                    session->sql(R"(UPDATE conversations SET latestSeq = ? WHERE conversationId = ?)")
                        .bind(accepted.conversationSeq)
                        .bind(accepted.conversationId)
                        .execute();
                    const std::string canonicalCommand = std::to_string(accepted.conversationId) + ":" +
                                                         std::to_string(static_cast<int>(Message::Type::TEXT)) + ":" +
                                                         content;
                    auto insertResult =
                        session
                            ->sql(
                                R"(INSERT INTO messages (senderId, receiverId, conversationId, conversationSeq, clientMessageId, commandHash, type, content, status, sendDateTime, readDateTime) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ''))")
                            .bind(senderId)
                            .bind(groupId)
                            .bind(accepted.conversationId)
                            .bind(accepted.conversationSeq)
                            .bind(clientMessageId)
                            .bind(std::to_string(std::hash<std::string>{}(canonicalCommand)))
                            .bind(static_cast<int>(Message::Type::TEXT))
                            .bind(content)
                            .bind(static_cast<int>(Message::Status::WAIT))
                            .bind(sendDateTime)
                            .execute();
                    accepted.messageId = static_cast<int64_t>(insertResult.getAutoIncrementValue());
                }

                auto members = session->sql(R"(SELECT uid FROM groupMembers WHERE groupId = ? AND uid <> ?)")
                                   .bind(groupId)
                                   .bind(senderId)
                                   .execute();
                for (const auto& row : members.fetchAll())
                    accepted.recipientUids.push_back(row[0].get<int64_t>());
                session->commit();
                accepted.error = ErrorCodes::Success;
                return accepted;
            } catch (const std::exception& error) {
                try {
                    session->rollback();
                } catch (...) {
                }
                accepted.error = ErrorCodes::MysqlFailed;
                accepted.diagnostic = error.what();
                return accepted;
            }
        });
    }

    ConversationSyncResult syncConversation(long uid, long conversationId, long afterSeq, int pullCount)
    {
        if (uid <= 0 || conversationId <= 0 || afterSeq < 0)
            return ConversationSyncResult{ErrorCodes::JsonParser};
        const int boundedCount = std::clamp(pullCount > 0 ? pullCount : 50, 1, 100);
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> ConversationSyncResult {
            ConversationSyncResult sync;
            auto membership =
                session
                    ->sql(
                        R"(SELECT c.latestSeq, m.joinedSeq, CAST(COALESCE(m.leftSeq, c.latestSeq) AS SIGNED), c.type FROM conversations c INNER JOIN conversationUserStates m ON m.conversationId = c.conversationId WHERE c.conversationId = ? AND m.uid = ?)")
                    .bind(conversationId)
                    .bind(uid)
                    .execute();
            auto memberRow = membership.fetchOne();
            if (!memberRow) {
                sync.error = ErrorCodes::MessageOwnershipInvalid;
                return sync;
            }

            sync.latestSeq = memberRow[0].get<int64_t>();
            const int64_t joinedSeq = memberRow[1].get<int64_t>();
            const int64_t leftSeq = memberRow[2].get<int64_t>();
            sync.conversationType = memberRow[3].get<int>();
            const int64_t effectiveAfter = std::max(afterSeq, joinedSeq - 1);
            auto result =
                session
                    ->sql(
                        R"(SELECT m.messageId,
                                  m.senderId,
                                  m.receiverId,
                                  m.conversationId,
                                  m.conversationSeq,
                                  COALESCE(m.clientMessageId, ''),
                                  m.type,
                                  m.content,
                                  m.status,
                                  m.sendDateTime,
                                  m.readDateTime,
                                  COALESCE(f.fileId, ''),
                                  COALESCE(f.fileName, ''),
                                  COALESCE(f.fileType, ''),
                                  CAST(COALESCE(f.fileSize, 0) AS SIGNED),
                                  f.sha256,
                                  CASE
                                      WHEN f.fileId IS NOT NULL
                                           AND (f.state <> 1 OR f.expiresTime <= ?) THEN 1
                                      ELSE 0
                                  END
                           FROM messages m
                           LEFT JOIN fileObjects f
                               ON m.type = 5 AND f.fileId = m.content
                           WHERE m.conversationId = ?
                             AND m.conversationSeq > ?
                             AND m.conversationSeq <= ?
                           ORDER BY m.conversationSeq ASC
                           LIMIT ?)")
                    .bind(getCurrentDateTime())
                    .bind(conversationId)
                    .bind(effectiveAfter)
                    .bind(leftSeq)
                    .bind(boundedCount + 1)
                    .execute();
            for (const auto& row : result.fetchAll()) {
                if (sync.messages.size() >= static_cast<std::size_t>(boundedCount)) {
                    sync.hasMore = true;
                    break;
                }
                ConversationMessageRecord message;
                message.messageId = row[0].get<int64_t>();
                message.senderId = row[1].get<int64_t>();
                message.receiverId = row[2].get<int64_t>();
                message.conversationId = row[3].get<int64_t>();
                message.conversationSeq = row[4].get<int64_t>();
                message.clientMessageId = row[5].get<std::string>();
                message.type = row[6].get<int>();
                message.content = row[7].get<std::string>();
                message.status = row[8].get<int>();
                message.sendDateTime = row[9].get<std::string>();
                message.readDateTime = row[10].get<std::string>();

                if (!row[11].get<std::string>().empty()) {
                    ConversationMessageRecord::FileAttachment attachment;
                    attachment.fileId = row[11].get<std::string>();
                    attachment.fileName = row[12].get<std::string>();
                    attachment.fileType = row[13].get<std::string>();
                    attachment.fileSize = row[14].get<int64_t>();

                    const auto sha256 = row[15].get<mysqlx::bytes>();
                    attachment.sha256.assign(reinterpret_cast<const char*>(sha256.begin()), sha256.size());
                    attachment.expired = row[16].get<int64_t>() != 0;
                    message.file = std::move(attachment);
                }
                sync.nextSeq = message.conversationSeq;
                sync.messages.push_back(std::move(message));
            }
            if (sync.messages.empty())
                sync.nextSeq = effectiveAfter;
            sync.error = ErrorCodes::Success;
            return sync;
        });
    }

    int updateUserInfoName(long uid, const std::string& name)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f = R"(UPDATE userInfo SET name = ? WHERE uid = ?)";
            auto result = session->sql(f).bind(name).bind(uid).execute();

            return 0;
        });
    }
    int updateUserInfoAge(long uid, int age)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f = R"(UPDATE userInfo SET age = ? WHERE uid = ?)";
            auto result = session->sql(f).bind(uid).bind(age).execute();

            return 0;
        });
    }
    int updateUserInfoSex(long uid, const std::string& sex)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f = R"(UPDATE userInfo SET sex = ? WHERE uid = ?)";
            auto result = session->sql(f).bind(sex).bind(uid).execute();

            return 0;
        });
    }
    int updateUserInfoHeadImageURL(long uid, const std::string& headImageURL)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f = R"(UPDATE userInfo SET headImageURL = ? WHERE uid = ?)";
            auto result = session->sql(f).bind(headImageURL).bind(uid).execute();

            return 0;
        });
    }
    int updateMessage(long messageId, short status, const std::string& readDateTime = "")
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            if (readDateTime.empty()) {
                std::string f = R"(UPDATE messages SET status = ? WHERE messageId = ?)";
                auto result = session->sql(f).bind(status).bind(messageId).execute();
            } else {
                std::string f = R"(UPDATE messages SET status = ?, readDateTime = ? WHERE messageId = ?)";
                auto result = session->sql(f).bind(status).bind(readDateTime).bind(messageId).execute();
            }

            return 0;
        });
    }

    int advanceConversationCursor(long uid, long conversationId, long conversationSeq, bool read)
    {
        if (uid <= 0 || conversationId <= 0 || conversationSeq <= 0)
            return 0;
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            auto result =
                session
                    ->sql(
                        R"(UPDATE conversationUserStates m INNER JOIN conversations c ON c.conversationId = m.conversationId SET m.deliveredSeq = GREATEST(m.deliveredSeq, ?), m.readSeq = CASE WHEN ? THEN GREATEST(m.readSeq, ?) ELSE m.readSeq END WHERE m.uid = ? AND m.conversationId = ? AND ? <= c.latestSeq)")
                    .bind(conversationSeq)
                    .bind(read)
                    .bind(conversationSeq)
                    .bind(uid)
                    .bind(conversationId)
                    .bind(conversationSeq)
                    .execute();
            return result.getAffectedItemsCount() > 0 ? 1 : 0;
        });
    }

    ConversationAckResult acknowledgeConversationMessage(long messageId, long uid, const std::string& deviceId,
                                                         long conversationId, long conversationSeq, short status,
                                                         const std::string& readDateTime = "")
    {
        if (messageId <= 0 || uid <= 0 || deviceId.empty() || conversationId <= 0 || conversationSeq <= 0)
            return {.updated = 0};
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> ConversationAckResult {
            ConversationAckResult acknowledged;
            session->startTransaction();
            try {
                auto owner =
                    session
                        ->sql(
                            R"(SELECT c.type, msg.receiverId, msg.senderId, msg.status FROM messages msg INNER JOIN conversations c ON c.conversationId = msg.conversationId INNER JOIN conversationUserStates cm ON cm.conversationId = c.conversationId AND cm.uid = ? INNER JOIN devices d ON d.uid = cm.uid AND d.deviceId = ? WHERE msg.messageId = ? AND msg.conversationId = ? AND msg.conversationSeq = ? FOR UPDATE)")
                        .bind(uid)
                        .bind(deviceId)
                        .bind(messageId)
                        .bind(conversationId)
                        .bind(conversationSeq)
                        .execute();
                auto ownerRow = owner.fetchOne();
                if (!ownerRow) {
                    session->rollback();
                    acknowledged.updated = 0;
                    return acknowledged;
                }
                const int conversationType = ownerRow[0].get<int>();
                const bool directRecipient = conversationType == 1 && ownerRow[1].get<int64_t>() == uid;
                acknowledged.senderUid = ownerRow[2].get<int64_t>();
                acknowledged.directConversation = conversationType == 1;
                acknowledged.readStateChanged = directRecipient && status == Message::Status::READ &&
                                                ownerRow[3].get<int>() < Message::Status::READ;

                if (directRecipient) {
                    session
                        ->sql(
                            R"(UPDATE messages SET readDateTime = CASE WHEN ? = 3 AND (status < 3 OR readDateTime IS NULL OR readDateTime = '') THEN ? ELSE readDateTime END, status = GREATEST(status, ?) WHERE messageId = ?)")
                        .bind(status)
                        .bind(readDateTime)
                        .bind(status)
                        .bind(messageId)
                        .execute();
                }
                session
                    ->sql(
                        R"(UPDATE conversationUserStates SET deliveredSeq = GREATEST(deliveredSeq, ?), readSeq = CASE WHEN ? = 3 THEN GREATEST(readSeq, ?) ELSE readSeq END WHERE uid = ? AND conversationId = ?)")
                    .bind(conversationSeq)
                    .bind(status)
                    .bind(conversationSeq)
                    .bind(uid)
                    .bind(conversationId)
                    .execute();
                session
                    ->sql(
                        R"(INSERT INTO conversationDeviceStates (conversationId, deviceId, persistedSeq) VALUES (?, ?, ?) ON DUPLICATE KEY UPDATE persistedSeq = GREATEST(persistedSeq, VALUES(persistedSeq)))")
                    .bind(conversationId)
                    .bind(deviceId)
                    .bind(conversationSeq)
                    .execute();
                session->commit();
                acknowledged.updated = 1;
                return acknowledged;
            } catch (...) {
                try {
                    session->rollback();
                } catch (...) {
                }
                throw;
            }
        });
    }
    FriendApply::FriendApplyGroup getFriendApplyList(long from)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> FriendApply::FriendApplyGroup {
            const static short STATUS = 0;
            std::string f = R"(SELECT * FROM friendApplys WHERE status = ? AND fromUid = ?)";
            auto result = session->sql(f).bind(STATUS).bind(from).execute();
            if (!(result.count() > 0)) {
                // log...
                LOG_DEBUG(dbLogger, "Mysql指令调用的结果集为空");
                return nullptr;
            }
            FriendApply::FriendApplyGroup friendApplyGroup(new std::vector<FriendApply::Ptr>());
            FriendApply::Ptr friendApply;
            for (auto row : result.fetchAll()) {
                size_t fromUid = row[0].get<size_t>();
                size_t toUid = row[1].get<size_t>();
                std::string content = row[2].get<std::string>();
                short status = row[3].get<int>();
                std::string createTime = row[4].get<std::string>();
                friendApply.reset(new FriendApply(fromUid, toUid, status, content, createTime));
                friendApplyGroup->push_back(friendApply);
            }
            return friendApplyGroup;
        });
    }

    int deleteUser(long uid)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f1 = R"(DELETE FROM users WHERE uid = ?)";
            std::string f2 = R"(DELETE FROM userInfo WHERE uid = ?)";
            auto result = session->sql(f1).bind(uid).execute();
            result = session->sql(f2).bind(uid).execute();
            return 0;
        });
    }
    int deleteFriendApply(long fromUid, long toUid)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f = R"(DELETE FROM friendApplys WHERE fromUid = ? AND toUid = ?)";
            auto result = session->sql(f).bind(fromUid).bind(toUid).execute();
            return 0;
        });
    }
    int deleteFriend(long uidA, long uidB)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            auto result = session->sql(R"(DELETE FROM friends WHERE uidA = ? AND uidB = ?)")
                              .bind(std::min(uidA, uidB))
                              .bind(std::max(uidA, uidB))
                              .execute();
            return 0;
        });
    }

    // todo...
    int deleteMessage(long uid, int lastMessageId, int delCount)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int { return 0; });
    }

    int hasEmail(std::string email)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f = R"(SELECT 1 FROM users WHERE email = ?)";
            auto result = session->sql(f).bind(email).execute();
            return (result.fetchOne() != 0);
        });
    }
    int hasUsername(std::string username)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f = R"(SELECT 1 FROM users WHERE username = ?)";
            auto result = session->sql(f).bind(username).execute();
            return (result.fetchOne() != 0);
        });
    }

    int insertGroup(GroupManager::Ptr group)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            session->startTransaction();
            try {
                session->sql(R"(INSERT INTO groupInfo (groupId, name, createTime) VALUES (?, ?, ?))")
                    .bind(group->groupId)
                    .bind(group->name)
                    .bind(group->createTime)
                    .execute();
                session
                    ->sql(
                        R"(INSERT INTO conversations (conversationId, type, businessId, latestSeq, createTime) VALUES (?, 2, ?, 0, ?))")
                    .bind(group->conversationId)
                    .bind(group->groupId)
                    .bind(group->createTime)
                    .execute();
                session->commit();
                return 0;
            } catch (...) {
                try {
                    session->rollback();
                } catch (...) {
                }
                throw;
            }
        });
    }

    int insertGroupMember(GroupMember::Ptr member)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            session->startTransaction();
            try {
                session
                    ->sql(
                        R"(INSERT IGNORE INTO groupMembers (groupId, uid, role, joinTime, speech, memberName) VALUES (?, ?, ?, ?, ?, ?))")
                    .bind(member->groupId)
                    .bind(member->uid)
                    .bind(member->role)
                    .bind(member->joinTime)
                    .bind(member->speech)
                    .bind(member->memberName)
                    .execute();
                session
                    ->sql(
                        R"(INSERT IGNORE INTO conversationUserStates (conversationId, uid, joinedSeq) SELECT conversationId, ?, latestSeq + 1 FROM conversations WHERE type = 2 AND businessId = ?)")
                    .bind(member->uid)
                    .bind(member->groupId)
                    .execute();
                session->commit();
                return 0;
            } catch (...) {
                try {
                    session->rollback();
                } catch (...) {
                }
                throw;
            }
        });
    }

    GroupMember::MemberList getGroupList(long uid)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> GroupMember::MemberList {
            std::string f = R"(SELECT * FROM groupMembers)";
            auto result = session->sql(f).execute();
            GroupMember::MemberList memberList;
            for (auto row : result.fetchAll()) {
                size_t groupId = row[0].get<size_t>();
                size_t uid = row[1].get<size_t>();
                short role = row[2].get<int>();
                std::string joinTime = row[3].get<std::string>();
                short speech = row[4].get<short>();
                std::string memberName = row[5].get<std::string>();
                GroupMember::Ptr member(new GroupMember(groupId, uid, role, joinTime, speech, memberName));
                memberList.push_back(member);
            }
            return memberList;
        });
    }

    int hasGroup(long groupId)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            auto result = session->sql(R"(SELECT 1 FROM groupInfo WHERE groupId = ?)").bind(groupId).execute();
            return (result.fetchOne() != 0);
        });
    }

    GroupMember::MemberList getGroupRoleMemberList(long groupId, short role)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> GroupMember::MemberList {
            // role
            // 大于等于是因为，管理往上的级别按数值排序，若2为管理，3则是大于管理的级别，如群主
            std::string f = R"(SELECT * FROM groupMembers WHERE groupId = ? AND role >= ?)";
            auto result = session->sql(f).bind(groupId).bind(role).execute();
            GroupMember::MemberList memberList;
            for (auto row : result.fetchAll()) {
                size_t rowGroupId = row[0].get<size_t>();
                size_t uid = row[1].get<size_t>();
                short role = row[2].get<int>();
                std::string joinTime = row[3].get<std::string>();
                short speech = row[4].get<int>();
                std::string memberName = row[5].get<std::string>();
                GroupMember::Ptr member(new GroupMember(rowGroupId, uid, role, joinTime, speech, memberName));
                memberList.push_back(member);
            }
            return memberList;
        });
    }
    GroupMember::MemberList getGroupMemberList(long groupId)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> GroupMember::MemberList {
            std::string f = R"(SELECT * FROM groupMembers WHERE groupId = ?)";
            auto result = session->sql(f).bind(groupId).execute();
            GroupMember::MemberList memberList;
            for (auto row : result.fetchAll()) {
                size_t rowGroupId = row[0].get<size_t>();
                size_t uid = row[1].get<size_t>();
                short role = row[2].get<int>();
                std::string joinTime = row[3].get<std::string>();
                short speech = row[4].get<int>();
                std::string memberName = row[5].get<std::string>();
                GroupMember::Ptr member(new GroupMember(rowGroupId, uid, role, joinTime, speech, memberName));
                memberList.push_back(member);
            }
            return memberList;
        });
    }

    int insertGroupApply(GroupApply::Ptr apply)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) {
            std::string f = R"(INSERT INTO `groupApplys` VALUES (?, ?, ?, ? , ?, ?, ?))";
            auto result = session->sql(f)
                              .bind(apply->requestor)
                              .bind(apply->handler)
                              .bind(apply->groupId)
                              .bind(apply->type)
                              .bind(apply->status)
                              .bind(apply->message)
                              .bind(apply->updateTime)
                              .execute();
            return 0;
        });
    }

    GroupApply::GroupApplyList selectGroupApply()
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> GroupApply::GroupApplyList {
            std::string f = R"(SELECT * FROM `groupApplys`)";
            auto result = session->sql(f).execute();
            if (!(result.count() > 0)) {
                LOG_INFO(dbLogger, "Mysql指令调用的结果集为空");
                return nullptr;
            }
            GroupApply::GroupApplyList applyList(new std::vector<GroupApply::Ptr>());
            for (auto row : result.fetchAll()) {
                size_t requestor = row[0].get<size_t>();
                size_t handler = row[1].get<size_t>();
                size_t groupId = row[2].get<size_t>();
                short type = row[3].get<int>();
                short status = row[4].get<int>();
                std::string message = row[5].get<std::string>();
                std::string updateTime = row[6].get<std::string>();
                GroupApply::Ptr apply(new GroupApply(requestor, handler, groupId, type, status, message, updateTime));
                applyList->push_back(apply);
            }
            return applyList;
        });
    }

    int hasGroupApply(long requestor, long groupId)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f = R"(SELECT 1 FROM groupApplys WHERE requestor = ? AND groupId = ?)";
            auto result = session->sql(f).bind(requestor).bind(groupId).execute();
            return (result.fetchOne() != 0);
        });
    }

    int deleteGroupApply(long requestor, long groupId)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> int {
            std::string f = R"(DELETE FROM groupApplys WHERE requestor = ? AND groupId = ?)";
            auto result = session->sql(f).bind(requestor).bind(groupId).execute();
            return 0;
        });
    }

    int updateGroupApply(GroupApply::Ptr apply)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) {
            std::string f =
                R"(UPDATE groupApplys SET handler = ?, status = ?, type = ?, message = ?, updateTime = ? WHERE requestor = ? AND groupId = ?)";
            auto result = session->sql(f)
                              .bind(apply->handler)
                              .bind(apply->status)
                              .bind(apply->type)
                              .bind(apply->message)
                              .bind(apply->updateTime)
                              .bind(apply->requestor)
                              .bind(apply->groupId)
                              .execute();
            return 0;
        });
    }

    GroupApply::GroupApplyList pullGroupApply(long requestor)
    {
        return executeTemplate([&](std::unique_ptr<mysqlx::Session>& session) -> GroupApply::GroupApplyList {
            std::string f = R"(SELECT * FROM `groupApplys` WHERE requestor = ?)";
            auto result = session->sql(f).bind(requestor).execute();
            if (!(result.count() > 0)) {
                LOG_INFO(dbLogger, "Mysql指令调用的结果集为空");
                return nullptr;
            }
            GroupApply::GroupApplyList applyList(new std::vector<GroupApply::Ptr>());
            for (auto row : result.fetchAll()) {
                size_t requestor = row[0].get<size_t>();
                size_t handler = row[1].get<size_t>();
                size_t groupId = row[2].get<size_t>();
                short type = row[3].get<int>();
                short status = row[4].get<int>();
                std::string message = row[5].get<std::string>();
                std::string updateTime = row[6].get<std::string>();
                GroupApply::Ptr apply(new GroupApply(requestor, handler, groupId, type, status, message, updateTime));
                applyList->push_back(apply);
            }
            return applyList;
        });
    }
};
}; // namespace wimi::db
