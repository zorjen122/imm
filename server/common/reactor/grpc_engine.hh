#pragma once
#include <grpc/event_engine/event_engine.h>
#include <seastar/core/future.hh>
#include <memory>

namespace imm::io
{
// 在 shard 0、任何 Channel 创建前初始化；各 shard 的网络与回调清理结束后，
// 先在 reactor 内 drain，再在 app.run() 返回后 shutdown，以免线程 join 阻塞 reactor。
void initialize_grpc();
seastar::future<> drain_grpc();
void shutdown_grpc();
// 本 shard 的所有 Channel 都已关闭后调用；等待残余 endpoint、DNS 和回调退出。
seastar::future<> drain_local_grpc();
std::shared_ptr<grpc_event_engine::experimental::EventEngine> make_grpc_engine();
// 调用方先取消并等待自己的 RPC，再释放 stub/channel，最后等待 engine 清理。
seastar::future<> close_grpc_engine(std::shared_ptr<grpc_event_engine::experimental::EventEngine> engine);
} // namespace imm::io
