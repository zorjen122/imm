#include "api/protocol.hh"
#include "api/email.hh"
#include "Logger.h"
#include "Const.h"
#include <google/protobuf/util/json_util.h>
#include <seastar/core/coroutine.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <array>
#include <algorithm>

namespace wimi::gate_protocol
{
namespace
{
template <typename Request> bool parse(std::string_view body, Request& request)
{
    request.Clear();
    // ProtoJSON 将 optional 字段的 null 视为未提供，因此解码成功后仍须检查 has_*，
    // 并校验取值；默认解析选项会拒绝未知字段及无法转换为目标标量类型的值。
    return google::protobuf::util::JsonStringToMessage(std::string(body), &request).ok();
}
bool credentials(const std::string& username, const std::string& password)
{
    return !username.empty() && username.size() <= 64 && !password.empty() && password.size() <= 128;
}
template <typename Request> int validate_verification(Request& request)
{
    if (!request.has_username() || !request.has_password() || !request.has_email() || !request.has_verifycode() ||
        !credentials(request.username(), request.password())) {
        return ErrorCodes::JsonParser;
    }
    request.set_email(normalize_email(request.email()));
    if (!is_valid_email(request.email())) {
        return ErrorCodes::EmailNotMatch;
    }
    const auto& code = request.verifycode();
    if (code.size() != 6 || !std::ranges::all_of(code, [](char digit) { return digit >= '0' && digit <= '9'; })) {
        return ErrorCodes::VerificationCodeInvalid;
    }
    return 0;
}
http_result error_result(unsigned status, int code)
{
    gate_api::error_response response;
    response.set_error(code);
    return {status, encode(response)};
}
http_result health(std::string_view)
{
    gate_api::health_response response;
    response.set_message("[TEST]");
    return {200, encode(response)};
}
template <typename Request>
seastar::future<http_result> invoke(gate_business::service& service, std::string body,
                                    seastar::scheduling_group transaction_group)
{
    // permit 在整个协程作用域内持有请求数、原始 payload 字节额度及请求 gate。
    // 解码失败、业务异常和正常响应编码完成都会归还额度；等待依赖时仍占用业务容量。
    auto permit = service.admit(body.size());
    Request request;
    if (const int error = decode(body, request)) {
        co_return error_result(200, error);
    }
    // 这里只切换本 shard 的 CPU 调度组，不把请求移交给其他线程或 shard。
    // 请求 message 按值交给业务协程；等待结束后本协程回到原组继续编码响应。
    auto response =
        co_await seastar::with_scheduling_group(transaction_group, [&service, request = std::move(request)]() mutable {
            return service.handle(std::move(request));
        });
    co_return http_result{200, encode(response)};
}
// API 的方法、路径和处理方式集中注册；invoke<Request> 通过重载绑定校验和业务函数。
// 新增 API 时补充 protobuf message、decode/handle 重载并在此注册，无须新增 kind 分支。
const std::array registry{
    route{"GET", "/test", immediate_handler{health}},
    route{"POST", "/post-verifycode", asynchronous_handler{invoke<gate_api::verification_request>}},
    route{"POST", "/post-signIn", asynchronous_handler{invoke<gate_api::sign_in_request>}},
    route{"POST", "/post-arrhythmia", asynchronous_handler{invoke<gate_api::gateway_request>}},
    route{"POST", "/post-signUp", asynchronous_handler{invoke<gate_api::sign_up_request>}},
    route{"POST", "/post-forget-password", asynchronous_handler{invoke<gate_api::reset_password_request>}},
};
} // namespace

std::span<const route> routes() { return registry; }
std::string encode(const google::protobuf::Message& response)
{
    std::string body;
    // 使用标准 ProtoJSON：int64 输出十进制字符串，字段名采用 lowerCamelCase；
    // optional 字段只在已设置时输出，因此成功响应也需要显式设置 error = 0。
    const auto status = google::protobuf::util::MessageToJsonString(response, &body);
    if (!status.ok()) {
        throw std::runtime_error("API response encoding failed: " + status.ToString());
    }
    return body;
}
int decode(std::string_view body, gate_api::verification_request& request)
{
    if (!parse(body, request) || !request.has_email()) {
        return ErrorCodes::JsonParser;
    }
    request.set_email(normalize_email(request.email()));
    return is_valid_email(request.email()) ? 0 : ErrorCodes::EmailNotMatch;
}
int decode(std::string_view body, gate_api::sign_in_request& request)
{
    if (!parse(body, request) || !request.has_username() || !request.has_password() ||
        !credentials(request.username(), request.password())) {
        return ErrorCodes::JsonParser;
    }
    return 0;
}
int decode(std::string_view body, gate_api::gateway_request& request)
{
    if (!parse(body, request) || !request.has_uid()) {
        return ErrorCodes::JsonParser;
    }
    return request.uid() > 0 ? 0 : ErrorCodes::UidInvalid;
}
int decode(std::string_view body, gate_api::sign_up_request& request)
{
    return parse(body, request) ? validate_verification(request) : ErrorCodes::JsonParser;
}
int decode(std::string_view body, gate_api::reset_password_request& request)
{
    return parse(body, request) ? validate_verification(request) : ErrorCodes::JsonParser;
}
seastar::future<http_result> dispatch(const route& route_endpoint, gate_business::service& service, std::string body,
                                      seastar::scheduling_group transaction_group)
{
    try {
        // 立即处理入口不申请异步业务额度；业务满载时，健康检查仍可在协议组完成。
        if (const auto* immediate = std::get_if<immediate_handler>(&route_endpoint.handler)) {
            co_return (*immediate)(body);
        } else if (const auto* asynchronous = std::get_if<asynchronous_handler>(&route_endpoint.handler)) {
            co_return co_await (*asynchronous)(service, std::move(body), transaction_group);
        } else {
            LOG_WARN(logger, "[gate/api] unknow the route");
        }
    } catch (const io::overloaded&) {
        co_return error_result(503, ErrorCodes::ResourceExhausted);
    } catch (const io::stopped&) {
        co_return error_result(503, ErrorCodes::DependencyUnavailable);
    } catch (const std::exception& error) {
        LOG_WARN(logger, "[gate/api] request failed: {}", error.what());
        co_return error_result(200, ErrorCodes::DependencyUnavailable);
    }
}
} // namespace wimi::gate_protocol
