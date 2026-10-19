#include "io/message_link.hh"
#include "io/driver.hh"
#include "Logger.h"
#include <grpc/impl/channel_arg_names.h>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <cassert>

namespace wimi::gateway_io
{
message_link::message_link(driver& owner, options config, message_node_id node_id, io::endpoint address)
    : _driver(owner), _options(std::move(config)), _node_id(std::move(node_id)), _address(std::move(address)),
      _engine(io::make_grpc_engine())
{
    namespace ee = grpc_event_engine::experimental;
    grpc::ChannelArguments arguments;
    arguments.SetInt(GRPC_ARG_ENABLE_HTTP_PROXY, 0);
    arguments.SetInt(GRPC_ARG_SERVICE_CONFIG_DISABLE_RESOLUTION, 1);
    arguments.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    arguments.SetPointerWithVtable(GRPC_ARG_EVENT_ENGINE, &_engine, ee::grpc_event_engine_arg_vtable());
    _channel = grpc::CreateCustomChannel("dns:///" + _address.host + ":" + std::to_string(_address.port),
                                         _options.message_credentials, arguments);
    _stub = gateway::GatewayMessageTransport::NewStub(_channel);
}
message_link::~message_link() { assert(_finished && !_engine); }
void message_link::check_owner() const { assert(seastar::engine_is_ready() && seastar::this_shard_id() == _owner); }
void message_link::start()
{
    check_owner();
    _stub->async()->Connect(_context.get(), this);
    AddHold();
    StartRead(&_read);
    gateway::GatewayToMessageFrame frame;
    auto* registration = frame.mutable_register_gateway();
    registration->set_protocol_version(1);
    registration->set_gateway_id(_options.gateway_id);
    registration->set_instance_id(_options.instance_id);
    registration->set_shard_id(_owner);
    registration->set_stream_epoch(std::chrono::steady_clock::now().time_since_epoch().count());
    registration->set_capacity(_options.max_writes);
    // 注册帧在 StartCall 之前排入，与命令/心跳共用该 shard 的发送预算。
    try {
        enqueue(std::move(frame));
    } catch (...) {
        stop();
    }
    StartCall();
}
void message_link::enqueue(gateway::GatewayToMessageFrame frame)
{
    check_owner();
    if (_stopping) {
        throw io::stopped();
    }
    auto units = _driver.writes.acquire(frame.ByteSizeLong());
    auto lifetime = _writers.hold();
    // 在提交处按顺序预约唯一写槽位，避免任务进入调度组的先后影响帧顺序。
    // 等待者由 Seastar 管理，帧和预算由协程持有；不再维护第二份发送容器。
    auto slot = seastar::get_units(_write_slot, 1);
    (void)seastar::with_scheduling_group(
        _io_group,
        [this, frame = std::move(frame), units = std::move(units), slot = std::move(slot)]() mutable {
            return write(std::move(frame), std::move(units), std::move(slot));
        })
        .handle_exception([this](std::exception_ptr error) {
            if (!_stopping) {
                LOG_WARN(logger, "[gateway/stream] write failed: {}", error);
                stop();
            }
        })
        .finally([lifetime = std::move(lifetime)] {});
}
seastar::future<> message_link::write(gateway::GatewayToMessageFrame frame, capacity_units units,
                                      seastar::future<seastar::semaphore_units<>> slot)
{
    auto exclusive = co_await std::move(slot);
    if (_stopping) {
        co_return;
    }
    assert(seastar::current_scheduling_group() == _io_group && !_write_done);
    seastar::promise<bool> complete;
    auto done = complete.get_future();
    _write_done = &complete;
    StartWrite(&frame);
    // 取消只中断等待槽位的任务，正在写的帧必须等待完成回调后才能释放。
    if (!co_await std::move(done)) {
        stop();
    }
}
void message_link::stop()
{
    check_owner();
    if (_stopping) {
        return;
    }
    _stopping = true;
    _healthy = false;
    _write_slot.broken(io::stopped());
    _context->TryCancel();
    RemoveHold();
}
void message_link::OnReadDone(bool ok)
{
    check_owner();
    if (!ok) {
        stop();
        return;
    }
    if (_read.has_register_result()) {
        _healthy = _read.register_result().accepted();
        if (!_healthy) {
            stop();
            return;
        }
    }
    if (_read.has_drain_notice()) {
        stop();
        return;
    }
    try {
        _driver.on_grpc_frame(*this, std::move(_read));
    } catch (const std::exception& error) {
        LOG_WARN(logger, "[gateway/stream] callback failed: {}", error.what());
        stop();
        return;
    }
    if (!_stopping) {
        StartRead(&_read);
    }
}
void message_link::OnWriteDone(bool ok)
{
    check_owner();
    assert(_write_done);
    auto* complete = std::exchange(_write_done, nullptr);
    if (!ok) {
        stop();
    }
    complete->set_value(ok);
}
void message_link::OnDone(const grpc::Status&)
{
    check_owner();
    _healthy = false;
    _finished = true;
    _driver.on_grpc_done(*this);
    _done.set_value();
}
seastar::future<> message_link::close()
{
    auto done = _done.get_future();
    stop();
    co_await std::move(done);
    // OnDone 保证 gRPC 不再借用任何帧；gate 进一步等待被唤醒的发送协程
    // 归还槽位和 shard 预算，完成异常处理后才能销毁流及其驱动资源。
    co_await _writers.close();
    // OnDone 已结束所有流回调，再释放 Context 持有的 call 引用；否则即使
    // Channel 外层指针已清空，内部传输与定时器仍可能被 call 保留。
    _context.reset();
    _stub.reset();
    _channel.reset();
    co_await io::close_grpc_engine(_engine);
    _engine.reset();
}
} // namespace wimi::gateway_io
