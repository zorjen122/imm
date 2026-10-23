#pragma once
#include "file.grpc.pb.h"
#include "options.hh"
#include "protocol.hh"
#include "storage.hh"
#include "reactor/dns_resolver.hh"
#include "reactor/grpc_server.hh"
#include <seastar/core/gate.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/shared_ptr.hh>
#include <functional>
#include <unordered_map>

namespace imm::file_service
{
struct scheduling_groups {
    seastar::scheduling_group protocol;
    seastar::scheduling_group transaction;
    seastar::scheduling_group io;
};
class unary_call;
class service final : public ::file::FileService::CallbackService, public seastar::peering_sharded_service<service>
{
    struct object_state {
        seastar::semaphore exclusive{1};
    };
    const options _options;
    const scheduling_groups _groups;
    io::dns_resolver _dns;
    io::grpc_server _server;
    storage _storage;
    seastar::semaphore _requests;
    seastar::semaphore _request_bytes;
    seastar::semaphore _operations;
    seastar::semaphore _operation_bytes;
    seastar::gate _calls;
    seastar::gate _tasks;
    seastar::gate _storage_tasks;
    std::unordered_map<std::string, seastar::lw_shared_ptr<object_state>> _objects;
    bool _stopping{};
    bool _stopped{};
    seastar::future<result> store(request operation, std::size_t bytes);
    seastar::future<response> dispatch(request operation);
    seastar::future<> execute(unary_call* call, request operation, std::size_t bytes,
                              std::function<void(response)> deliver, seastar::gate::holder lifetime);
    template <typename Request, typename Response>
    grpc::ServerUnaryReactor* submit(grpc::CallbackServerContext& context, const Request& input, Response& output);

  public:
    service(options config, scheduling_groups groups);
    seastar::future<> start();
    seastar::future<> stop();
    grpc::ServerUnaryReactor* Upload(grpc::CallbackServerContext*, const ::file::UploadRequest*,
                                     ::file::UploadResponse*) override;
    grpc::ServerUnaryReactor* Download(grpc::CallbackServerContext*, const ::file::DownloadRequest*,
                                       ::file::DownloadResponse*) override;
};
} // namespace imm::file_service
