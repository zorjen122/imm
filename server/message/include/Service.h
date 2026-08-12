#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <spdlog/spdlog.h>

#include "Const.h"
#include "ClientForwardService.h"
#include "FileService.h"
#include "FriendService.h"
#include "GroupService.h"
#include "MessageService.h"
#include "TcpMessageCodec.h"
#include "ThreadPool.h"

namespace wimi
{

namespace rpc
{
class GatewayStreamService;
}

class Service : public Singleton<Service>
{
    friend class Singleton<Service>;

  public:
    using HandleType = std::function<TcpPacket(uint32_t, int64_t, TcpPacket&)>;

    ~Service();
    bool PostBackgroundTask(ThreadPool::Task task);
    ClientForwardService& ClientForwards();
    MessageService& Messages();
    void SetGatewayStreamService(rpc::GatewayStreamService* service);
    TcpPacket ExecuteGatewayCommand(uint32_t msgID, int64_t actorUid, TcpPacket request);

  private:
    enum class TaskType { Light, Heavy };
    struct HandlerEntry {
        HandleType handle;
        TaskType taskType{TaskType::Heavy};
    };

    Service();
    void Init();
    void RegisterHandle(uint32_t msgID, TaskType taskType, HandleType handle);

    ClientForwardService clientForwardService;
    FriendService friendService;
    GroupService groupService;
    MessageService messageService;
    FileService fileService;
    std::unique_ptr<ThreadPool> threadPool;
    std::atomic<uint64_t> lightDispatched{0};
    std::atomic<uint64_t> heavyDispatched{0};
    std::atomic<uint64_t> heavyRejected{0};
    std::chrono::milliseconds requestTimeout{3000};
    std::chrono::milliseconds queueAcquireTimeout{2};
    std::map<uint32_t, HandlerEntry> serviceGroup;
};
}; // namespace wimi
