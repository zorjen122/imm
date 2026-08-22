#include "Service.h"

#include "Const.h"
#include "Logger.h"
#include "Metrics.h"

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
    LOG_INFO(businessLogger, "[message/dispatcher-start] 业务调度器已启动, handlers: {}, workers: {}",
             serviceGroup.size(), workerCount);
}

Service::~Service()
{
    if (threadPool) {
        auto stats = threadPool->GetStats();
        LOG_INFO(businessLogger,
                 "[message/dispatcher-stop] 业务调度器已停止, workers: {}, queued: {}, thread_pool "
                 "submitted/completed/rejected/acquire-timeout/expired: "
                 "{}/{}/{}/{}/{}, requests started/succeeded/failed/expired: "
                 "{}/{}/{}/{}",
                 stats.workerCount, stats.queueSize, stats.submitted, stats.completed, stats.rejected,
                 stats.acquireTimeouts, stats.expired, Metrics::Get(Metric::RequestsStarted),
                 Metrics::Get(Metric::RequestsSucceeded), Metrics::Get(Metric::RequestsFailed),
                 Metrics::Get(Metric::RequestsExpired));
        threadPool->Stop();
    }
}

void Service::RegisterHandle(uint32_t msgID, TaskType taskType, HandleType handle)
{
    if (serviceGroup.find(msgID) != serviceGroup.end()) {
        LOG_WARN(businessLogger, "[message/register-handler] 重复的服务处理器, service_id: {}", msgID);
        return;
    }
    serviceGroup[msgID] = HandlerEntry{std::move(handle), taskType};
}

void Service::Init()
{
    // 状态

    // 消息与文件
    RegisterHandle(ID_TEXT_SEND_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return messageService.SendText(msgID, actorUid, request);
    });
    RegisterHandle(ID_FILE_UPLOAD_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return fileService.Upload(msgID, actorUid, request);
    });
    RegisterHandle(ID_FILE_UPLOAD_CHUNK_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return fileService.Upload(msgID, actorUid, request);
    });
    RegisterHandle(ID_FILE_UPLOAD_COMPLETE_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return fileService.Upload(msgID, actorUid, request);
    });
    RegisterHandle(ID_FILE_DOWNLOAD_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return fileService.Download(msgID, actorUid, request);
    });

    // 好友
    RegisterHandle(ID_NOTIFY_ADD_FRIEND_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return friendService.NotifyAddFriend(msgID, actorUid, request);
    });
    RegisterHandle(ID_REPLY_ADD_FRIEND_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return friendService.ReplyAddFriend(msgID, actorUid, request);
    });
    RegisterHandle(ID_PULL_FRIEND_LIST_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return friendService.PullFriendList(msgID, actorUid, request);
    });
    RegisterHandle(ID_PULL_FRIEND_APPLY_LIST_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return friendService.PullFriendApplyList(msgID, actorUid, request);
    });

    // 消息拉取
    RegisterHandle(ID_PULL_SESSION_MESSAGE_LIST_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return messageService.PullConversationMessages(msgID, actorUid, request);
    });
    // 群聊
    RegisterHandle(ID_GROUP_CREATE_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return groupService.Create(msgID, actorUid, request);
    });
    RegisterHandle(ID_GROUP_NOTIFY_JOIN_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return groupService.NotifyJoin(msgID, actorUid, request);
    });
    RegisterHandle(ID_GROUP_REPLY_JOIN_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return groupService.ReplyJoin(msgID, actorUid, request);
    });
    RegisterHandle(ID_GROUP_QUIT_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return groupService.Quit(msgID, actorUid, request);
    });
    RegisterHandle(ID_GROUP_KICK_MEMBER_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return groupService.Kick(msgID, actorUid, request);
    });
    RegisterHandle(ID_GROUP_PULL_MEMBER_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return groupService.PullNotify(msgID, actorUid, request);
    });

    /* 待实现 */
    RegisterHandle(ID_GROUP_TEXT_SEND_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return messageService.SendGroupText(msgID, actorUid, request);
    });
    RegisterHandle(ID_FILE_SEND_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return messageService.SendFile(msgID, actorUid, request);
    });
    RegisterHandle(ID_GROUP_FILE_SEND_REQ, TaskType::Heavy, [this](auto msgID, auto actorUid, auto& request) {
        return messageService.SendGroupFile(msgID, actorUid, request);
    });
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

TcpPacket Service::ExecuteGatewayCommand(uint32_t msgID, int64_t actorUid, TcpPacket request)
{
    if (actorUid <= 0 || msgID == ID_LOGIN_INIT_REQ || msgID == ID_PING_REQ || msgID == ID_USER_QUIT_REQ) {
        LOG_WARN(businessLogger, "[message/dispatch] 拒绝 Gateway 控制命令, uid: {}, service_id: {}", actorUid, msgID);
        return MakeErrorPacket(ErrorCodes::AuthenticationRequired, "connection control command belongs to gateway");
    }
    auto handle = serviceGroup.find(msgID);
    if (handle == serviceGroup.end()) {
        LOG_WARN(businessLogger, "[message/dispatch] 未找到服务处理器, uid: {}, service_id: {}", actorUid, msgID);
        return MakeErrorPacket(ErrorCodes::NotFound);
    }
    LOG_DEBUG(businessLogger, "[message/dispatch] 开始处理请求, uid: {}, service_id: {}", actorUid, msgID);
    return handle->second.handle(msgID, actorUid, request);
}

} // namespace wimi
