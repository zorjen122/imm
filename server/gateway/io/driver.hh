#pragma once
#include "io/operations.hh"
#include "io/options.hh"
#include "io/budget.hh"
#include "io/types.hh"
#include "reactor/dns_resolver.hh"
#include "reactor/redis_connection.hh"
#include "reactor/mysql_connection.hh"
#include "state.grpc.pb.h"
#include <seastar/core/abort_source.hh>
#include <seastar/core/condition-variable.hh>
#include <set>
#include <functional>
#include <unordered_map>

namespace imm::gateway_io
{
class message_link;
class driver
{
    struct pending_command {
        message_link* link;
        std::string client_request_id;
        capacity_units units;
        seastar::promise<gateway::CommandResult> completion;
    };
    options _options;
    io::dns_resolver _dns;
    std::set<io::redis_connection*> _redis;
    std::set<io::mysql_connection*> _mysql;
    std::shared_ptr<grpc_event_engine::experimental::EventEngine> _state_engine;
    std::shared_ptr<grpc::Channel> _state_channel;
    std::unique_ptr<state::StateService::Stub> _state;
    grpc::ClientContext* _state_call{};
    // driver 由每个 shard 的 transaction service 独占；本表按 Message 节点 ID
    // 持有独立的 gRPC link。拓扑稳定且连接建立后，M 个 shard、N 个节点对应
    // M × N 条流；重连替换该节点的 link，拓扑增删同步调整本表。
    std::unordered_map<message_node_id, std::unique_ptr<message_link>> _message_links;
    std::unordered_map<std::string, pending_command> _pending;
    budget _pending_budget;
    seastar::abort_source _abort;
    seastar::condition_variable _grpc_changed;
    std::optional<seastar::future<>> _grpc_control;
    std::optional<seastar::future<>> _dns_closed;
    bool _mysql_initialized{};
    bool _stopping{};
    std::function<gateway::ClientForwardStatus(const gateway::ClientForwardEnvelope&)> _forward;
    seastar::future<io::redis_reply> redis_command(std::vector<std::string> command);
    seastar::future<io::endpoint> discover_redis_master();
    seastar::future<io::mysql_result> mysql_query(std::string sql, std::vector<io::mysql_parameter> parameters);
    seastar::future<state::MessageTopology> fetch_grpc_topology();
    seastar::future<> grpc_control();
    message_link& select_grpc_link(int64_t conversation_id);

  public:
    budget writes;
    budget reliable;
    explicit driver(options config);
    ~driver();
    seastar::future<> start(std::function<gateway::ClientForwardStatus(const gateway::ClientForwardEnvelope&)> forward);
    seastar::future<> wait_ready();
    void abort();
    void cancel(const std::string& connection_id);
    seastar::future<> stop();
    void on_grpc_frame(message_link& link, gateway::MessageToGatewayFrame frame);
    void on_grpc_done(message_link& link);
    seastar::future<bool> execute(validate_token);
    seastar::future<bool> execute(prepare_profile);
    seastar::future<bool> execute(register_device);
    seastar::future<bound_session> execute(bind_session);
    seastar::future<bool> execute(refresh_session);
    seastar::future<> execute(clear_session);
    seastar::future<command_ticket> execute(send_command);
};
} // namespace imm::gateway_io
