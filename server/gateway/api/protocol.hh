#pragma once
#include "TcpMessageCodec.h"
#include <span>
#include <string_view>
#include <seastar/core/future.hh>

namespace wimi::gateway_transport
{
class connection;
}

namespace wimi::gateway_protocol
{
enum class processing { login, immediate, forward };
struct route {
    protocol::Operation operation;
    processing mode;
    seastar::future<> (gateway_transport::connection::*handle)(protocol::ClientRequest);
};
std::span<const route> routes();
// 解码完整 LV 正文；这里只检查信封和包类型，连接鉴权及依赖状态由业务层判断。
ClientEnvelope decode(std::string_view payload);
const route& route_for(protocol::Operation operation);
int validate(const protocol::ClientRequest& request);
} // namespace wimi::gateway_protocol
