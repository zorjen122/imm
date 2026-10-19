#pragma once
#include "transaction/service.hh"
#include <google/protobuf/message.h>
#include <seastar/core/scheduling.hh>
#include <span>
#include <string_view>
#include <variant>

namespace wimi::gate_protocol
{
struct http_result {
    unsigned status{200};
    std::string body;
};
struct scheduling_groups {
    seastar::scheduling_group protocol;
    seastar::scheduling_group transaction;
    seastar::scheduling_group io;
};
using immediate_handler = http_result (*)(std::string_view);
using asynchronous_handler = seastar::future<http_result> (*)(gate_transaction::service&, std::string,
                                                              seastar::scheduling_group);
struct route {
    std::string_view method;
    std::string_view path;
    std::variant<immediate_handler, asynchronous_handler> handler;
};

// HTTP 路由的唯一注册表。handler 类型区分立即完成的短计算与可能挂起的业务协程；
// scheduling group 只分配 CPU 份额，立即处理函数同样不得执行阻塞 I/O。
std::span<const route> routes();
// 将 ProtoJSON 转成对应请求，检查字段存在性、取值边界并规范化输入。
// 返回 0 才能交给业务层；非零值是 API 错误码，此时不得使用部分解析的请求执行业务。
int decode(std::string_view body, gate_api::verification_request& request);
int decode(std::string_view body, gate_api::sign_in_request& request);
int decode(std::string_view body, gate_api::gateway_request& request);
int decode(std::string_view body, gate_api::sign_up_request& request);
int decode(std::string_view body, gate_api::reset_password_request& request);
std::string encode(const google::protobuf::Message& response);
seastar::future<http_result> dispatch(const route& route_endpoint, gate_transaction::service& service, std::string body,
                                      seastar::scheduling_group transaction_group);
} // namespace wimi::gate_protocol
