#include "business/service.hh"
#include "Logger.h"
#include "Const.h"
#include <seastar/core/coroutine.hh>
#include <sys/random.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <array>
#include <random>
#include <system_error>
#include <cassert>

namespace wimi::gate_business
{
using namespace gate_io;
namespace
{
// 使用非阻塞系统熵源，调用留在当前 reactor；熵源不可用则报告失败，
// 不以可预测的伪随机状态替代验证码和认证 token 所需的随机性。
void random_bytes(void* data, size_t length)
{
    auto* cursor = static_cast<char*>(data);
    while (length) {
        const auto count = getrandom(cursor, length, GRND_NONBLOCK);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::system_category(), "authentication entropy unavailable");
        }
        cursor += count;
        length -= count;
    }
}
struct random_source {
    using result_type = uint32_t;
    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return UINT32_MAX; }
    result_type operator()()
    {
        result_type value;
        random_bytes(&value, sizeof(value));
        return value;
    }
};
std::string verification_code()
{
    random_source source;
    auto code = std::to_string(std::uniform_int_distribution<unsigned>(0, 999999)(source));
    code.insert(code.begin(), 6 - code.size(), '0');
    return code;
}
std::string chat_token()
{
    std::array<unsigned char, 32> data;
    random_bytes(data.data(), data.size());
    const char* digits = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : data) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

template <typename Response> Response result(int error = 0)
{
    Response response;
    response.set_error(error);
    return response;
}
} // namespace
seastar::future<gate_api::verification_response> service::handle(gate_api::verification_request request)
{
    assert(seastar::current_scheduling_group() == _transaction_group);
    if (!_options.email_enabled && !_options.expose_code) {
        co_return result<gate_api::verification_response>(ErrorCodes::DependencyUnavailable);
    }
    const auto email = request.email();
    auto code = verification_code();
    const bool issued =
        co_await _io.submit(issue_verification_code{email, code, _options.code_ttl_seconds, _options.cooldown_seconds});
    if (issued && _options.email_enabled) {
        std::exception_ptr failure;
        try {
            co_await _io.submit(send_verification_email{email, code, _options.code_ttl_seconds});
        } catch (...) {
            failure = std::current_exception();
        }
        if (failure) {
            // Redis 与 SMTP 不构成事务；仅在仍等于本次验证码时清理，避免删除后写入的值。
            // 未收到确认也不能证明邮件未投递，所以不自动重发；清理失败则记录并等待 TTL。
            try {
                co_await _io.submit(clear_verification_code{email, code});
            } catch (const std::exception& error) {
                LOG_WARN(logger, "[gate/business] verification cleanup failed: {}", error.what());
            }
            std::rethrow_exception(failure);
        }
    }
    if (!issued && _options.expose_code) {
        code = co_await _io.submit(get_verification_code{email});
    }
    auto response = result<gate_api::verification_response>();
    response.set_email(email);
    response.set_issued(issued);
    if (_options.expose_code) {
        response.set_verification_code(std::move(code));
    }
    co_return response;
}
seastar::future<gate_api::sign_in_response> service::handle(gate_api::sign_in_request request)
{
    assert(seastar::current_scheduling_group() == _transaction_group);
    auto account = co_await _io.submit(find_user{request.username()});
    if (!account || account->password != request.password()) {
        co_return result<gate_api::sign_in_response>(-1);
    }
    const auto node = co_await _io.submit(pick_gateway{account->uid});
    const bool initialized = co_await _io.submit(has_user_info{account->uid});
    auto token = chat_token();
    constexpr int token_ttl_seconds = 15 * 60;
    co_await _io.submit(set_chat_token{account->uid, token, token_ttl_seconds});
    auto response = result<gate_api::sign_in_response>();
    response.set_init(initialized ? 0 : 1);
    response.set_uid(account->uid);
    response.set_ip(node.host);
    response.set_port(node.port);
    response.set_gateway_id(node.id);
    response.set_chat_token(std::move(token));
    response.set_chat_token_expires_in(token_ttl_seconds);
    co_return response;
}
seastar::future<gate_api::gateway_response> service::handle(gate_api::gateway_request request)
{
    assert(seastar::current_scheduling_group() == _transaction_group);
    const auto node = co_await _io.submit(pick_gateway{request.uid()});
    auto response = result<gate_api::gateway_response>();
    response.set_host(node.host);
    response.set_port(node.port);
    co_return response;
}
seastar::future<gate_api::sign_up_response> service::handle(gate_api::sign_up_request request)
{
    assert(seastar::current_scheduling_group() == _transaction_group);
    if (co_await _io.submit(user_exists{request.username(), request.email()})) {
        co_return result<gate_api::sign_up_response>(ErrorCodes::UserExist);
    }
    if (!co_await _io.submit(consume_verification_code{request.email(), request.verifycode(), _options.max_attempts})) {
        co_return result<gate_api::sign_up_response>(ErrorCodes::VerificationCodeInvalid);
    }
    // 验证码消费与 MySQL 写入不是跨库事务；消费成功后若写入失败，验证码不会恢复。
    // 前面的存在性查询也不能排除并发注册，最终仍由数据库唯一约束裁决重复。
    const auto uid = co_await _io.submit(generate_user_id{_options.machine_id});
    if (!co_await _io.submit(register_user{uid, request.username(), request.password(), request.email()})) {
        co_return result<gate_api::sign_up_response>(ErrorCodes::UserExist);
    }
    auto response = result<gate_api::sign_up_response>();
    response.set_uid(uid);
    response.set_username(request.username());
    response.set_email(request.email());
    co_return response;
}
seastar::future<gate_api::reset_password_response> service::handle(gate_api::reset_password_request request)
{
    assert(seastar::current_scheduling_group() == _transaction_group);
    auto account = co_await _io.submit(find_user{request.username()});
    if (!account) {
        co_return result<gate_api::reset_password_response>(ErrorCodes::NotFound);
    }
    if (account->email != request.email()) {
        co_return result<gate_api::reset_password_response>(ErrorCodes::EmailNotMatch);
    }
    if (!co_await _io.submit(consume_verification_code{request.email(), request.verifycode(), _options.max_attempts})) {
        co_return result<gate_api::reset_password_response>(ErrorCodes::VerificationCodeInvalid);
    }
    co_await _io.submit(update_password{account->uid, request.password()});
    auto response = result<gate_api::reset_password_response>();
    response.set_username(request.username());
    co_return response;
}
} // namespace wimi::gate_business
