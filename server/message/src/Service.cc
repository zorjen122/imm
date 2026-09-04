#include "Service.h"

#include "Const.h"
#include "Logger.h"
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace wimi
{
namespace
{

std::size_t ResolveServiceWorkerCount()
{
    if (const char* value = std::getenv("WIMI_CHAT_SERVICE_WORKERS")) {
        int configured = std::atoi(value);
        if (configured > 0) {
            return static_cast<std::size_t>(configured);
        }
    }

    std::size_t hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
        hardware = 4;
    }
    return std::clamp<std::size_t>(hardware / 2, 2, 8);
}

} // namespace

Service::Service() : friendService(clientForwardService), groupService(clientForwardService)
{
    Init();
    const auto workerCount = ResolveServiceWorkerCount();
    threadPool = std::make_unique<ThreadPool>("message-biz", workerCount);
    LOG_INFO(logger, "[message/dispatcher-start] 业务调度器已启动, handlers: {}, workers: {}", serviceGroup.size(),
             workerCount);
}

Service::~Service()
{
    if (threadPool) {
        auto stats = threadPool->GetStats();
        LOG_INFO(logger,
                 "[message/dispatcher-stop] 业务调度器已停止, workers: {}, queued: {}, "
                 "submitted/completed/rejected: {}/{}/{}",
                 stats.workerCount, stats.queueSize, stats.submitted, stats.completed, stats.rejected);
        threadPool->Stop();
    }
}

void Service::RegisterHandle(protocol::Operation operation, TaskType taskType, HandleType handle)
{
    if (serviceGroup.find(operation) != serviceGroup.end()) {
        LOG_WARN(logger, "[message/register-handler] 重复的服务处理器, operation: {}",
                 protocol::Operation_Name(operation));
        return;
    }
    serviceGroup[operation] = HandlerEntry{std::move(handle), taskType};
}

void Service::Init()
{
    // 状态

    // 消息与文件
    RegisterHandle(protocol::Operation::OP_UPLOAD_FILE, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return fileService.Upload(actorUid, request); });
    RegisterHandle(protocol::Operation::OP_UPLOAD_FILE_CHUNK, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return fileService.Upload(actorUid, request); });
    RegisterHandle(protocol::Operation::OP_COMPLETE_FILE_UPLOAD, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return fileService.Upload(actorUid, request); });
    RegisterHandle(protocol::Operation::OP_DOWNLOAD_FILE, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return fileService.Download(actorUid, request); });

    // 好友
    RegisterHandle(protocol::Operation::OP_ADD_FRIEND, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return friendService.NotifyAddFriend(actorUid, request); });
    RegisterHandle(protocol::Operation::OP_REPLY_FRIEND, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return friendService.ReplyAddFriend(actorUid, request); });
    RegisterHandle(protocol::Operation::OP_PULL_FRIEND_LIST, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return friendService.PullFriendList(actorUid, request); });
    RegisterHandle(
        protocol::Operation::OP_PULL_FRIEND_APPLICATIONS, TaskType::Heavy,
        [this](auto actorUid, auto& request) { return friendService.PullFriendApplyList(actorUid, request); });

    // 消息拉取
    RegisterHandle(
        protocol::Operation::OP_PULL_CONVERSATION_MESSAGES, TaskType::Heavy,
        [this](auto actorUid, auto& request) { return messageService.PullConversationMessages(actorUid, request); });
    // 群聊
    RegisterHandle(protocol::Operation::OP_CREATE_GROUP, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return groupService.Create(actorUid, request); });
    RegisterHandle(protocol::Operation::OP_JOIN_GROUP, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return groupService.NotifyJoin(actorUid, request); });
    RegisterHandle(protocol::Operation::OP_REPLY_JOIN_GROUP, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return groupService.ReplyJoin(actorUid, request); });
    RegisterHandle(protocol::Operation::OP_QUIT_GROUP, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return groupService.Quit(actorUid, request); });
    RegisterHandle(protocol::Operation::OP_KICK_GROUP_MEMBER, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return groupService.Kick(actorUid, request); });
    RegisterHandle(protocol::Operation::OP_PULL_GROUP_MEMBERS, TaskType::Heavy,
                   [this](auto actorUid, auto& request) { return groupService.PullNotify(actorUid, request); });
}

bool Service::PostBackgroundTask(ThreadPool::Task task)
{
    if (!threadPool) {
        return false;
    }
    return threadPool->Post(std::move(task));
}

MessageService& Service::Messages() { return messageService; }

void Service::SetGatewayStreamService(rpc::GatewayStreamService* service)
{
    clientForwardService.SetGatewayStreamService(service);
}

TcpPacket Service::ExecuteGatewayCommand(protocol::Operation operation, int64_t actorUid, TcpPacket request)
{
    if (actorUid <= 0 || operation == protocol::Operation::OP_LOGIN || operation == protocol::Operation::OP_PING ||
        operation == protocol::Operation::OP_QUIT) {
        LOG_WARN(logger, "[message/dispatch] 拒绝 Gateway 控制命令, uid: {}, operation: {}", actorUid,
                 protocol::Operation_Name(operation));
        return MakeErrorPacket(ErrorCodes::AuthenticationRequired, "connection control command belongs to gateway");
    }
    auto handle = serviceGroup.find(operation);
    if (handle == serviceGroup.end()) {
        LOG_WARN(logger, "[message/dispatch] 未找到服务处理器, uid: {}, operation: {}", actorUid,
                 protocol::Operation_Name(operation));
        return MakeErrorPacket(ErrorCodes::NotFound);
    }
    LOG_DEBUG(logger, "[message/dispatch] 开始处理请求, uid: {}, operation: {}", actorUid,
              protocol::Operation_Name(operation));
    return handle->second.handle(actorUid, request);
}

} // namespace wimi
