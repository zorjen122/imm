#include "MessageLinkManager.h"

#include "Configer.h"
#include "Const.h"
#include "GrpcSecurity.h"
#include "Logger.h"
#include "TcpMessageCodec.h"
#include "gateway_message.grpc.pb.h"
#include "state.grpc.pb.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <algorithm>
#include <chrono>
#include <utility>

namespace wimi::connection
{
namespace asio = boost::asio;
namespace
{

constexpr auto kForwardAttemptTimeout = std::chrono::milliseconds(1000);

int64_t NowUnixMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

MessageLinkManager::MessageLinkManager(asio::io_context& ioContext, asio::thread_pool& controlPool,
                                       std::string gatewayId, std::string instanceId)
    : ioContext(ioContext), controlPool(controlPool), gatewayId(std::move(gatewayId)), instanceId(std::move(instanceId))
{
    auto config = Configer::getNode("server");
    messageCredentials = BuildChannelCredentials(LoadGrpcSecurityConfig(config));
    if (config["stateRPC"]) {
        stateAddress =
            config["stateRPC"]["host"].as<std::string>() + ":" + config["stateRPC"]["port"].as<std::string>();
    }
}

MessageLinkManager::~MessageLinkManager() { Stop(); }

void MessageLinkManager::Start()
{
    if (stateAddress.empty()) {
        LOG_ERROR(logger, "[gateway/topology] 缺少 stateRPC 配置, 无法发现 Message 节点");
        return;
    }
    LOG_INFO(logger, "[gateway/topology] 开始同步 Message 节点, state_endpoint: {}", stateAddress);
    asio::co_spawn(ioContext, TopologyLoop(), asio::detached);
}

void MessageLinkManager::Stop()
{
    if (stopping.exchange(true))
        return;
    std::vector<std::shared_ptr<MessageLink>> current;
    {
        std::lock_guard<std::mutex> lock(linksMutex);
        for (auto& [_, link] : links)
            current.push_back(link);
    }
    for (auto& link : current)
        link->Stop();

    std::vector<PendingCommand> abandoned;
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        for (auto& [_, command] : pending)
            abandoned.push_back(std::move(command));
        pending.clear();
    }
    for (auto& command : abandoned) {
        if (command.attemptTimer)
            command.attemptTimer->cancel();
        if (!command.callback)
            continue;
        gateway::CommandResult result;
        result.set_request_id(command.command.request_id());
        result.set_error(ErrorCodes::DependencyUnavailable);
        result.set_retryable(true);
        result.set_packet(
            SerializeTcpPacket(MakeErrorPacket(ErrorCodes::DependencyUnavailable, "message links are stopping")));
        command.callback(result);
    }
    LOG_INFO(logger, "[gateway/message-links-stop] Message 链路已停止, links: {}, abandoned_commands: {}",
             current.size(), abandoned.size());
}

bool MessageLinkManager::Ready() const { return HealthyLinkCount() > 0; }

std::size_t MessageLinkManager::HealthyLinkCount() const
{
    std::lock_guard<std::mutex> lock(linksMutex);
    return std::count_if(links.begin(), links.end(), [](const auto& item) { return item.second->Healthy(); });
}

asio::awaitable<bool> MessageLinkManager::Forward(gateway::CommandEnvelope command, CommandCallback callback)
{
    co_return ForwardNow(std::move(command), std::move(callback));
}

bool MessageLinkManager::ForwardNow(gateway::CommandEnvelope command, CommandCallback callback)
{
    if (stopping.load(std::memory_order_acquire)) {
        LOG_DEBUG(logger, "[gateway/command-route] 链路正在停止, 拒绝新请求, request_id: {}", command.request_id());
        return false;
    }

    auto link = SelectLink(command.conversation_id());
    if (!link) {
        LOG_WARN(logger, "[gateway/command-route] 没有可用的 Message 链路, request_id: {}, operation: {}",
                 command.request_id(), static_cast<int>(command.operation()));
        return false;
    }

    const std::string requestId = command.request_id();
    auto attemptTimer = std::make_shared<asio::steady_timer>(ioContext);

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        if (pending.contains(requestId)) {
            LOG_WARN(logger, "[gateway/command-route] 拒绝重复 request_id, request_id: {}", requestId);
            return false;
        }

        PendingCommand pendingCommand;
        pendingCommand.command = command;
        pendingCommand.callback = std::move(callback);
        pendingCommand.attemptTimer = attemptTimer;
        pendingCommand.currentLinkToken = link->Token();
        pendingCommand.attempt = 1;
        pending.emplace(requestId, std::move(pendingCommand));
    }

    gateway::GatewayToMessageFrame frame;
    *frame.mutable_command() = std::move(command);

    const auto enqueueResult = link->Enqueue(std::move(frame));
    if (enqueueResult == MessageLink::EnqueueResult::Accepted) {
        link->IncrementInflight();
        LOG_DEBUG(logger, "[gateway/command-route] 请求已进入 Message 链路, request_id: {}, node: {}, attempt: 1",
                  requestId, link->Id());

        // 单次投递 deadline 仅在帧已进入链路写队列后开始计时。
        ArmAttemptTimer(requestId, 1);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        auto found = pending.find(requestId);
        if (found != pending.end()) {
            found->second.retryPending = true;

            if (enqueueResult == MessageLink::EnqueueResult::Stopped)
                found->second.ignoredLinkTokens.insert(link->Token());
        }
    }

    // Enqueue 返回失败意味着帧未进入该链路的写队列，可以安全地立即重新选择链路。
    RetryPending();

    return true;
}

void MessageLinkManager::SetClientForwardHandler(ClientForwardHandler handler)
{
    clientForwardHandler = std::move(handler);
}

asio::awaitable<void> MessageLinkManager::TopologyLoop()
{
    constexpr uint16_t __pollIntervalSeconds = 5;

    asio::steady_timer timer(ioContext);
    uint64_t heartbeatSequence = 0;

    // State 是 Message 链路拓扑的唯一来源；此处只同步 State 快照，不读取本地节点配置。
    while (!stopping.load(std::memory_order_acquire)) {
        auto snapshot = co_await asio::co_spawn(
            controlPool, [this]() -> asio::awaitable<TopologySnapshot> { co_return FetchTopology(); },
            asio::use_awaitable);
        if (snapshot.received)
            SyncTopologyLink(snapshot);

        std::vector<std::shared_ptr<MessageLink>> current;
        {
            std::lock_guard<std::mutex> lock(linksMutex);
            for (auto& [_, link] : links)
                current.push_back(link);
        }

        // 每 5 秒还会做两件事：给健康 link 发 heartbeat；
        // 如果某条 link 超过 30 秒没有读到任何帧，就主动 stop 它。

        const int64_t now = NowUnixMilliseconds();
        for (auto& link : current) {
            if (link->Healthy())
                link->Heartbeat(++heartbeatSequence);
            if (now - link->LastReadAt() > 30000) {
                LOG_WARN(logger, "[gateway/message-link] Message 链路响应超时, node: {}, idle_ms: {}", link->Id(),
                         now - link->LastReadAt());
                link->Stop();
            }
        }

        timer.expires_after(std::chrono::seconds(__pollIntervalSeconds));
        boost::system::error_code ec;
        co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
        if (ec == asio::error::operation_aborted)
            break;
    }
}

MessageLinkManager::TopologySnapshot MessageLinkManager::FetchTopology()
{
    TopologySnapshot snapshot;
    auto channel = grpc::CreateChannel(stateAddress, grpc::InsecureChannelCredentials());
    auto stub = state::StateService::NewStub(channel);
    state::TopologyRequest request;
    state::MessageTopology response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    auto status = stub->ListMessageNodes(&context, request, &response);
    if (!status.ok()) {
        LOG_WARN(logger, "[gateway/topology] 获取 Message 节点失败, error: {}", status.error_message());
        return snapshot;
    }
    snapshot.received = true;
    for (const auto& source : response.nodes()) {
        if (source.status() != "active" || source.host().empty() || source.port() <= 0)
            continue;
        snapshot.nodes.push_back(Node{source.node_id(), source.host(), static_cast<unsigned short>(source.port())});
    }
    LOG_DEBUG(logger, "[gateway/topology] Message 节点快照已更新, active_nodes: {}", snapshot.nodes.size());
    return snapshot;
}

void MessageLinkManager::SyncTopologyLink(const TopologySnapshot& snapshot)
{
    std::unordered_map<std::string, Node> snapshotNodes;
    for (const auto& node : snapshot.nodes)
        snapshotNodes[node.id] = node;

    std::vector<std::shared_ptr<MessageLink>> removed;
    {
        std::lock_guard<std::mutex> lock(linksMutex);
        for (auto current = links.begin(); current != links.end();) {
            const auto wanted = snapshotNodes.find(current->first);
            const auto configured = topologyNodes.find(current->first);
            const bool endpointChanged =
                wanted != snapshotNodes.end() && configured != topologyNodes.end() &&
                (wanted->second.host != configured->second.host || wanted->second.port != configured->second.port);
            if (wanted == snapshotNodes.end() || endpointChanged) {
                removed.push_back(current->second);
                AppendPendingDone(current->second);
                current = links.erase(current);
            } else {
                ++current;
            }
        }
        topologyNodes = snapshotNodes;
    }
    for (auto& link : removed)
        link->Stop();

    for (const auto& node : snapshot.nodes) {
        bool start = false;
        {
            std::lock_guard<std::mutex> lock(linksMutex);
            start = !links.contains(node.id);
        }
        if (start)
            StartLink(node);
    }
    if (!removed.empty())
        LOG_INFO(logger, "[gateway/topology] 已移除失效 Message 链路, removed: {}", removed.size());
}

void MessageLinkManager::StartLink(const Node& node)
{
    const std::string token = node.id + ":" + std::to_string(nextLinkToken.fetch_add(1, std::memory_order_relaxed));
    auto link = std::make_shared<MessageLink>(node, gatewayId, instanceId, token, *this);
    {
        std::lock_guard<std::mutex> lock(linksMutex);
        auto found = links.find(node.id);
        if (found != links.end()) {
            auto old = found->second;
            AppendPendingDone(old);
            old->Stop();
        }
        links[node.id] = link;
    }
    link->Start();
}

void MessageLinkManager::OnFrame(const std::string& nodeId, const gateway::MessageToGatewayFrame& frame)
{
    // CommandResult 通过 request_id 与在途命令配对，只完成一次回调。
    if (frame.has_command_result()) {
        PendingCommand command;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            auto pendingIt = pending.find(frame.command_result().request_id());
            if (pendingIt != pending.end()) {
                command = std::move(pendingIt->second);
                pending.erase(pendingIt);
                found = true;
            }
        }
        if (found) {
            if (command.attemptTimer)
                command.attemptTimer->cancel();
        }

        {
            std::lock_guard<std::mutex> lock(linksMutex);
            auto link = links.find(nodeId);
            if (link != links.end())
                link->second->DecrementInflight();
        }

        if (found && command.callback) {
            LOG_DEBUG(logger,
                      "[gateway/command-result] 收到 Message 处理结果, node: {}, "
                      "request_id: {}, error: {}, "
                      "retryable: {}",
                      nodeId, frame.command_result().request_id(), frame.command_result().error(),
                      frame.command_result().retryable());
            command.callback(frame.command_result());
        } else {
            LOG_WARN(logger, "[gateway/command-result] 忽略无法匹配的处理结果, node: {}, request_id: {}", nodeId,
                     frame.command_result().request_id());
        }
        return;
    }

    // RegisterResult 是流进入 healthy 的门禁；拒绝结果保留 unhealthy。
    if (frame.has_register_result()) {
        if (frame.register_result().accepted()) {
            LOG_INFO(logger,
                     "[gateway/message-link-register] Message 链路注册成功, node: {}, "
                     "message_node_id: {}, stream_epoch: {}",
                     nodeId, frame.register_result().message_node_id(), frame.register_result().stream_epoch());
            RetryPending();
        } else {
            LOG_ERROR(logger,
                      "[gateway/message-link-register] Message 链路注册失败, node: {}, "
                      "message_node_id: {}, stream_epoch: {}, reason: {}",
                      nodeId, frame.register_result().message_node_id(), frame.register_result().stream_epoch(),
                      frame.register_result().reason());
        }
        return;
    }

    // HeartbeatAck 的到达时间已在 OnReadDone 更新；这里只记录序号和往返延迟。
    if (frame.has_heartbeat_ack()) {
        LOG_TRACE(logger,
                  "[gateway/message-heartbeat] 收到 Message 心跳响应, node: {}, sequence: {}, "
                  "round_trip_ms: {}",
                  nodeId, frame.heartbeat_ack().sequence(),
                  std::max<int64_t>(0, NowUnixMilliseconds() - frame.heartbeat_ack().sent_at_unix_ms()));
        return;
    }

    // DrainNotice 立即把节点移出健康集合，禁止新命令继续路由到正在下线的流。
    if (frame.has_drain_notice()) {
        LOG_WARN(logger,
                 "[gateway/message-drain] Message 节点准备下线, node: {}, "
                 "message_node_id: {}, reason: {}",
                 nodeId, frame.drain_notice().message_node_id(), frame.drain_notice().reason());
        std::shared_ptr<MessageLink> link;
        {
            std::lock_guard<std::mutex> lock(linksMutex);
            auto found = links.find(nodeId);
            if (found != links.end())
                link = found->second;
        }
        if (link)
            link->Drain();
        return;
    }

    // ClientForward 先交给本地 SessionManager 做 generation 校验和物理推送，再沿
    // 原流返回 ClientForwardAck；业务消息已持久化，因此离线/背压不会回滚
    // ACCEPTED。
    if (frame.has_client_forward()) {
        LOG_DEBUG(logger,
                  "[gateway/client-forward] 收到客户端推送, node: {}, forward_id: {}, "
                  "recipient_uid: {}, message_id: {}, conversation_id: {}, "
                  "conversation_seq: {}",
                  nodeId, frame.client_forward().forward_id(), frame.client_forward().recipient_uid(),
                  frame.client_forward().message_id(), frame.client_forward().conversation_id(),
                  frame.client_forward().conversation_seq());
        gateway::ClientForwardStatus status = gateway::CLIENT_FORWARD_STATUS_OFFLINE;
        if (clientForwardHandler) {
            status = clientForwardHandler(frame.client_forward());
        } else {
            LOG_ERROR(logger,
                      "[gateway/client-forward] 客户端推送处理器未设置, node: {}, "
                      "forward_id: {}",
                      nodeId, frame.client_forward().forward_id());
        }
        std::shared_ptr<MessageLink> link;
        {
            std::lock_guard<std::mutex> lock(linksMutex);
            auto found = links.find(nodeId);
            if (found != links.end())
                link = found->second;
        }
        if (link) {
            gateway::GatewayToMessageFrame ackFrame;
            auto* ack = ackFrame.mutable_client_forward_ack();
            ack->set_forward_id(frame.client_forward().forward_id());
            ack->set_status(status);
            ack->set_gateway_id(gatewayId);
            ack->set_instance_id(instanceId);
            ack->set_recipient_device_id(frame.client_forward().recipient_device_id());
            if (link->Enqueue(std::move(ackFrame)) != MessageLink::EnqueueResult::Accepted) {
                LOG_WARN(logger,
                         "[gateway/client-forward-ack] 推送确认未进入 Message 写队列, node: {}, "
                         "forward_id: {}, status: {}",
                         nodeId, frame.client_forward().forward_id(), static_cast<int>(status));
            } else {
                LOG_DEBUG(logger,
                          "[gateway/client-forward-ack] 推送确认已进入 Message 写队列, node: {}, forward_id: {}, "
                          "status: {}",
                          nodeId, frame.client_forward().forward_id(), static_cast<int>(status));
            }
        } else {
            LOG_WARN(logger,
                     "[gateway/client-forward-ack] Message 链路不存在, 无法返回推送确认, "
                     "node: {}, forward_id: {}, status: {}",
                     nodeId, frame.client_forward().forward_id(), static_cast<int>(status));
        }
        return;
    }

    // oneof 理论上只会命中上述分支；空 frame 作为协议异常保留可观测性。
    LOG_WARN(logger, "[gateway/message-frame] 收到空 Message 数据帧, node: {}", nodeId);
}

void MessageLinkManager::OnLinkDone(const std::string& nodeId, MessageLink* source)
{
    asio::post(ioContext, [this, nodeId, source]() {
        if (stopping.load(std::memory_order_acquire))
            return;

        std::shared_ptr<MessageLink> completed;
        {
            std::lock_guard<std::mutex> lock(linksMutex);
            auto found = links.find(nodeId);
            if (found == links.end() || found->second.get() != source) {
                auto doneLink =
                    std::find_if(donePending.begin(), donePending.end(),
                                 [source](const std::shared_ptr<MessageLink>& link) { return link.get() == source; });

                if (doneLink == donePending.end())
                    return;

                completed = std::move(*doneLink);
                donePending.erase(doneLink);
            } else {
                completed = std::move(found->second);
                links.erase(found);
            }
        }
        RetryPending(completed->Token());
    });
}

void MessageLinkManager::OnLinkWritable()
{
    asio::post(ioContext, [this]() {
        if (stopping.load(std::memory_order_acquire))
            return;

        RetryPending();
    });
}

/*
RetryPending 的实现是以状态节点作为消息链路唯一拓扑来源，不会打破这个约定，关于重传有三个状态区分：
  - 已成功入队，等待结果；
  - 尚未入队或当前尝试超时，等待重新选路；
  - 正在执行重新选路和入队，避免两个事件并发重复调度。
如果成功写入但超时，不会切换其他链路重传。反之则切换链路重传。
*/
void MessageLinkManager::RetryPending(const std::string& failedLinkToken)
{
    if (stopping.load(std::memory_order_acquire))
        return;

    struct RetryCommand {
        std::string requestId;
        gateway::CommandEnvelope command;
        std::unordered_set<std::string> ignoreLinkTokens;
    };

    std::vector<RetryCommand> retries;
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        for (auto& [requestId, pendingCommand] : pending) {
            if (!failedLinkToken.empty()) {
                const bool canReplaySentCommand = pendingCommand.currentLinkToken == failedLinkToken;
                const bool wasRejectedBeforeEnqueue =
                    pendingCommand.currentLinkToken == failedLinkToken && pendingCommand.retryPending;

                if (canReplaySentCommand || wasRejectedBeforeEnqueue) {
                    pendingCommand.ignoredLinkTokens.insert(failedLinkToken);
                    pendingCommand.retryPending = true;
                }
            }

            if (!pendingCommand.retryPending || pendingCommand.retrying)
                continue;

            pendingCommand.retrying = true;

            auto ignoreLinkTokens = pendingCommand.ignoredLinkTokens;
            retries.push_back(RetryCommand{requestId, pendingCommand.command, std::move(ignoreLinkTokens)});
        }
    }

    bool redispatch = false;
    for (const auto& retry : retries) {
        // State 是拓扑唯一来源；重传只从 State 已创建且健康的链路中选择。
        auto link = SelectLink(retry.command.conversation_id(), retry.ignoreLinkTokens);
        auto enqueueResult = MessageLink::EnqueueResult::Stopped;
        bool enqueueAttempted = false;

        if (link) {
            gateway::GatewayToMessageFrame frame;
            *frame.mutable_command() = retry.command;

            enqueueAttempted = true;
            enqueueResult = link->Enqueue(std::move(frame));
            if (enqueueResult == MessageLink::EnqueueResult::Accepted)
                link->IncrementInflight();
        }

        std::uint64_t attempt = 0;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            auto found = pending.find(retry.requestId);
            if (found == pending.end() || !found->second.retrying)
                continue;

            found->second.retrying = false;

            if (enqueueAttempted && enqueueResult == MessageLink::EnqueueResult::Accepted) {
                found->second.currentLinkToken = link->Token();
                found->second.retryPending = false;
                attempt = ++found->second.attempt;
            } else if (enqueueAttempted) {
                found->second.retryPending = true;

                if (enqueueResult == MessageLink::EnqueueResult::Stopped)
                    found->second.ignoredLinkTokens.insert(link->Token());

                redispatch = true;
            }
        }

        if (attempt != 0)
            LOG_DEBUG(logger, "[gateway/command-retry] 请求已切换 Message 链路, request_id: {}, node: {}, attempt: {}",
                      retry.requestId, link->Id(), attempt);
        if (attempt != 0)
            ArmAttemptTimer(retry.requestId, attempt);
    }

    if (redispatch)
        RetryPending();
}

void MessageLinkManager::OnAttemptTimeout(const std::string& requestId, std::uint64_t attempt)
{
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        auto found = pending.find(requestId);
        if (found == pending.end() || found->second.attempt != attempt) {
            return;
        }

        found->second.ignoredLinkTokens.insert(found->second.currentLinkToken);
        found->second.retryPending = true;
    }

    LOG_WARN(logger, "[gateway/command-retry] Message 节点单次处理超时, request_id: {}, attempt: {}", requestId,
             attempt);
    RetryPending();
}

void MessageLinkManager::ArmAttemptTimer(const std::string& requestId, std::uint64_t attempt)
{
    std::shared_ptr<asio::steady_timer> timer;
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        auto found = pending.find(requestId);
        if (found == pending.end() || found->second.attempt != attempt)
            return;

        timer = found->second.attemptTimer;
    }

    timer->expires_after(kForwardAttemptTimeout);
    timer->async_wait([this, requestId, attempt](const boost::system::error_code& error) {
        if (!error)
            OnAttemptTimeout(requestId, attempt);
    });
}

void MessageLinkManager::AppendPendingDone(std::shared_ptr<MessageLink> link)
{
    donePending.push_back(std::move(link));
}

std::shared_ptr<MessageLink> MessageLinkManager::SelectLink(int64_t conversationId,
                                                            const std::unordered_set<std::string>& ignoredLinkTokens)
{
    std::vector<std::shared_ptr<MessageLink>> healthy;
    {
        std::lock_guard<std::mutex> lock(linksMutex);
        for (const auto& [_, link] : links) {
            if (link->Healthy() && link->Writable() && !ignoredLinkTokens.contains(link->Token()))
                healthy.push_back(link);
        }
    }
    if (healthy.empty())
        return {};

    if (conversationId > 0) {
        std::shared_ptr<MessageLink> selected;
        std::size_t best = 0;
        for (const auto& link : healthy) {
            auto score = std::hash<std::string>{}(std::to_string(conversationId) + ":" + link->Id());
            if (!selected || score > best) {
                best = score;
                selected = link;
            }
        }
        return selected;
    }

    return *std::min_element(healthy.begin(), healthy.end(),
                             [](const auto& left, const auto& right) { return left->Inflight() < right->Inflight(); });
}

} // namespace wimi::connection
