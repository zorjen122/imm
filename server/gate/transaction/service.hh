#pragma once
#include "gate_api.pb.h"
#include "IoQueue.h"
#include "io/driver.hh"
#include <seastar/core/gate.hh>
#include <seastar/core/semaphore.hh>

namespace wimi::gate_transaction
{

// 准入凭证覆盖解码、业务挂起及响应编码；获取额度不等待，service 必须存活到凭证销毁。
// 成员按声明的逆序析构：先归还字节和请求数额度，最后释放 gate，停止等待才可完成。
struct request_permit {
    seastar::gate::holder lifetime;
    seastar::semaphore_units<> count;
    seastar::semaphore_units<> bytes;
};

// 每个 shard 独占一个业务服务及其 I/O 依赖，所有状态只在所属 reactor 上访问。
// 调用 handle() 前必须持有 admit() 返回的凭证，并已由协议层完成请求校验；
// handle() 在 transaction 调度组运行，stop() 则拒绝新请求并等待既有请求及驱动退出。
class service
{
    gate_io::options _options;
    seastar::scheduling_group _transaction_group;
    gate_io::driver _driver;
    io::operation_queue<gate_io::driver, gate_io::io_operation> _io;
    seastar::gate _requests;
    seastar::semaphore _request_count;
    seastar::semaphore _request_bytes;
    bool _stopping{};
    bool _stopped{};

  public:
    service(gate_io::options config, seastar::scheduling_group transaction_group, seastar::scheduling_group io_group);
    seastar::future<> start();
    request_permit admit(size_t bytes);
    seastar::future<gate_api::verification_response> handle(gate_api::verification_request request);
    seastar::future<gate_api::sign_in_response> handle(gate_api::sign_in_request request);
    seastar::future<gate_api::gateway_response> handle(gate_api::gateway_request request);
    seastar::future<gate_api::sign_up_response> handle(gate_api::sign_up_request request);
    seastar::future<gate_api::reset_password_response> handle(gate_api::reset_password_request request);
    seastar::future<> stop();
};
} // namespace wimi::gate_transaction
