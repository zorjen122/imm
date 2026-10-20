#pragma once
#include "transport/server.hh"
#include "TcpMessageCodec.h"

namespace wimi::message_transaction
{
struct delivery {
    int64_t recipient_uid{};
    protocol::Packet packet;
    std::string excluded_device_id;
};
// 根请求协程持有上下文直至响应及后续投递结束；处理器只暂存推送，
// 不因提交后的路由失效或背压把已经成功的数据库操作改成失败。
struct request_context {
    message_transport::stream_route origin;
    gateway::CommandEnvelope command;
    protocol::Packet packet;
    std::vector<delivery> deliveries;
};
} // namespace wimi::message_transaction
