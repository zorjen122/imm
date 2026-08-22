#include "MessageLink.h"

#include "Logger.h"
#include "MessageLinkManager.h"

#include <grpcpp/create_channel.h>
#include <chrono>
#include <utility>

namespace wimi::connection
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

MessageLink::MessageLink(Node node, std::string gatewayId, std::string instanceId, std::string token,
                         MessageLinkManager& manager)
    : node(std::move(node)), gatewayId(std::move(gatewayId)), instanceId(std::move(instanceId)),
      token(std::move(token)), manager(manager), lastReadAt(NowUnixMilliseconds())
{
    auto channel =
        grpc::CreateChannel(this->node.host + ":" + std::to_string(this->node.port), manager.messageCredentials);
    stub = gateway::GatewayMessageTransport::NewStub(channel);
}

void MessageLink::Start()
{
    LOG_INFO(logger,
             "[gateway/message-link] 正在连接 Message 节点, node: {}, endpoint: {}:{}, "
             "gateway: {}, instance: {}",
             node.id, node.host, node.port, gatewayId, instanceId);
    stub->async()->Connect(&context, this);
    AddHold();
    externalHold.store(true, std::memory_order_release);
    StartRead(&readFrame);

    gateway::GatewayToMessageFrame frame;
    auto* registration = frame.mutable_register_gateway();
    registration->set_protocol_version(1);
    registration->set_gateway_id(gatewayId);
    registration->set_instance_id(instanceId);
    registration->set_stream_epoch(static_cast<uint64_t>(NowUnixMilliseconds()));
    registration->set_capacity(kMaxStreamQueue);
    Enqueue(std::move(frame));
    StartCall();
}

void MessageLink::Stop()
{
    if (stopped.exchange(true))
        return;

    healthy.store(false, std::memory_order_release);
    context.TryCancel();
    ReleaseHold();
}

MessageLink::EnqueueResult MessageLink::Enqueue(gateway::GatewayToMessageFrame frame)
{
    bool startWrite = false;

    {
        std::lock_guard<std::mutex> lock(writeMutex);
        if (stopped)
            return EnqueueResult::Stopped;

        if (writeQueue.size() >= kMaxStreamQueue) {
            LOG_WARN(logger, "[gateway/message-link] 写队列已满, node: {}, queued: {}", node.id, writeQueue.size());
            return EnqueueResult::QueueFull;
        }

        writeQueue.push(std::move(frame));
        if (!writeInFlight) {
            writeInFlight = true;
            writeFrame = std::move(writeQueue.front());
            writeQueue.pop();
            startWrite = true;
        }
    }

    if (startWrite)
        StartWrite(&writeFrame);

    return EnqueueResult::Accepted;
}

void MessageLink::Heartbeat(uint64_t sequence)
{
    gateway::GatewayToMessageFrame frame;
    auto* heartbeat = frame.mutable_heartbeat();
    heartbeat->set_sent_at_unix_ms(NowUnixMilliseconds());
    heartbeat->set_sequence(sequence);
    Enqueue(std::move(frame));
}

bool MessageLink::Healthy() const { return healthy.load(std::memory_order_acquire); }

bool MessageLink::Writable() const
{
    std::lock_guard<std::mutex> lock(writeMutex);
    return !stopped && writeQueue.size() < kMaxStreamQueue;
}

std::size_t MessageLink::Inflight() const { return inflight.load(std::memory_order_relaxed); }

void MessageLink::IncrementInflight() { inflight.fetch_add(1, std::memory_order_relaxed); }

void MessageLink::DecrementInflight()
{
    auto current = inflight.load(std::memory_order_relaxed);
    while (current > 0 && !inflight.compare_exchange_weak(current, current - 1, std::memory_order_relaxed)) {
    }
}

const std::string& MessageLink::Id() const { return node.id; }

const std::string& MessageLink::Token() const { return token; }

void MessageLink::Drain()
{
    healthy.store(false, std::memory_order_release);
    Stop();
}

int64_t MessageLink::LastReadAt() const { return lastReadAt.load(std::memory_order_relaxed); }

void MessageLink::OnReadDone(bool ok)
{
    if (!ok) {
        LOG_WARN(logger, "[gateway/message-link] Message 读取通道已关闭, node: {}, gateway: {}", node.id, gatewayId);
        stopped.store(true, std::memory_order_release);
        healthy.store(false, std::memory_order_release);
        ReleaseHold();
        return;
    }
    lastReadAt.store(NowUnixMilliseconds(), std::memory_order_relaxed);
    if (readFrame.has_register_result())
        healthy.store(readFrame.register_result().accepted(), std::memory_order_release);

    // 传递给 manager 处理 packet
    manager.OnFrame(node.id, readFrame);
    readFrame.Clear();

    StartRead(&readFrame);
}

void MessageLink::OnWriteDone(bool ok)
{
    if (!ok) {
        LOG_WARN(logger, "[gateway/message-link] Message 写入通道已关闭, node: {}, gateway: {}", node.id, gatewayId);
        stopped.store(true, std::memory_order_release);
        healthy.store(false, std::memory_order_release);
        ReleaseHold();
        return;
    }

    bool startWrite = false;
    bool becameWritable = false;

    {
        std::lock_guard<std::mutex> lock(writeMutex);
        if (!writeQueue.empty()) {
            becameWritable = writeQueue.size() == kMaxStreamQueue;
            writeFrame = std::move(writeQueue.front());
            writeQueue.pop();
            startWrite = true;
        } else {
            writeInFlight = false;
        }
    }

    if (startWrite)
        StartWrite(&writeFrame);

    if (becameWritable)
        manager.OnLinkWritable();
}

void MessageLink::OnDone(const grpc::Status& status)
{
    stopped.store(true, std::memory_order_release);
    healthy.store(false, std::memory_order_release);
    if (status.ok()) {
        LOG_INFO(logger, "[gateway/message-link] Message 链路已关闭, node: {}", node.id);
    } else {
        LOG_WARN(logger, "[gateway/message-link] Message 链路异常关闭, node: {}, error: {}", node.id,
                 status.error_message());
    }
    manager.OnLinkDone(node.id, this);
}

void MessageLink::ReleaseHold()
{
    bool expected = true;
    if (externalHold.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
        RemoveHold();
}

} // namespace wimi::connection
