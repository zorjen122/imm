#include "gateway_message.grpc.pb.h"
#include <grpc/impl/channel_arg_names.h>
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <future>
#include <iostream>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
int positive_number(const std::string& text, int maximum)
{
    std::size_t consumed{};
    const int value = std::stoi(text, &consumed);
    if (consumed != text.size() || value < 1 || value > maximum) {
        throw std::invalid_argument("expected integer in 1.." + std::to_string(maximum));
    }
    return value;
}

double elapsed_ms(std::chrono::steady_clock::time_point begin)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

std::shared_ptr<grpc::Channel> connect(const std::string& endpoint)
{
    grpc::ChannelArguments arguments;
    // 每个客户端使用独立 subchannel pool，避免多个 channel 合并为同一条 HTTP/2 连接。
    arguments.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    return grpc::CreateCustomChannel(endpoint, grpc::InsecureChannelCredentials(), arguments);
}

struct result {
    uint64_t sent_bytes{};
    uint64_t received_bytes{};
    uint64_t success{};
    double rtt_ms{};
    std::string error;
};

result run_connection(std::string endpoint, std::string gateway_id, std::string instance_id, unsigned shard_id,
                      int sent_nr)
{
    result result;
    try {
        auto stub = imm::gateway::GatewayMessageTransport::NewStub(connect(endpoint));
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
        auto stream = stub->Connect(&context);

        imm::gateway::GatewayToMessageFrame outgoing;
        const auto epoch = std::chrono::system_clock::now().time_since_epoch().count();
        auto* registration = outgoing.mutable_register_gateway();
        registration->set_protocol_version(1);
        registration->set_gateway_id(gateway_id);
        registration->set_instance_id(instance_id);
        registration->set_stream_epoch(epoch);
        registration->set_shard_id(shard_id);
        registration->set_capacity(1);

        imm::gateway::MessageToGatewayFrame incoming;
        if (!stream->Write(outgoing) || !stream->Read(&incoming) || !incoming.has_register_result() ||
            !incoming.register_result().accepted() ||
            incoming.register_result().stream_epoch() != static_cast<uint64_t>(epoch)) {
            context.TryCancel();
            const auto status = stream->Finish();
            throw std::runtime_error("registration failed; check Message owner port and gateway identity: " +
                                     status.error_message());
        }

        for (int index = 1; index <= sent_nr; ++index) {
            outgoing.Clear();
            auto* heartbeat = outgoing.mutable_heartbeat();
            heartbeat->set_sequence(index);
            heartbeat->set_sent_at_unix_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::system_clock::now().time_since_epoch())
                                               .count());

            const auto begin = std::chrono::steady_clock::now();
            if (!stream->Write(outgoing) || !stream->Read(&incoming) || !incoming.has_heartbeat_ack() ||
                incoming.heartbeat_ack().sequence() != static_cast<uint64_t>(index) ||
                incoming.heartbeat_ack().sent_at_unix_ms() != heartbeat->sent_at_unix_ms()) {
                context.TryCancel();
                const auto status = stream->Finish();
                throw std::runtime_error("unexpected frame or heartbeat failed: " + status.error_message());
            }
            result.sent_bytes += outgoing.ByteSizeLong() + 5;
            result.received_bytes += incoming.ByteSizeLong() + 5;

            ++result.success;
            result.rtt_ms += elapsed_ms(begin);
        }
        stream->WritesDone();
        // 测量结束后取消长流，并等待 Finish 回收底层调用。
        context.TryCancel();
        const auto status = stream->Finish();
        if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED) {
            throw std::runtime_error(status.error_message());
        }
    } catch (const std::exception& error) {
        result.error = error.what();
    }

    return result;
}
} // namespace

int main(int argc, char** argv)
{
    try {
        std::string endpoint = "127.0.0.1:50055";
        int sent_nr = 100;
        int connection_nr = 1;
        std::string gateway_id = "benchmark";

        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--help") {
                std::cout
                    << "bm_message [--endpoint host:owner-port] [--sent_nr N] [--connection_nr N] [--gateway_id ID]\n";
                return 0;
            }
            if (++index == argc) {
                throw std::invalid_argument("missing value for " + option);
            }

            const std::string value = argv[index];
            if (option == "--endpoint") {
                endpoint = value;
            } else if (option == "--sent_nr") {
                sent_nr = positive_number(value, 100000);
            } else if (option == "--connection_nr") {
                connection_nr = positive_number(value, 256);
            } else if (option == "--gateway_id") {
                gateway_id = value;
            } else {
                throw std::invalid_argument("unknown option: " + option);
            }
        }

        if (endpoint.empty()) {
            throw std::invalid_argument("endpoint must be nonempty");
        }

        if (gateway_id.empty()) {
            throw std::invalid_argument("gateway_id must be nonempty");
        }
        const auto instance_id =
            "benchmark-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::vector<std::future<result>> connections;
        const auto begin = std::chrono::steady_clock::now();
        for (int index = 0; index < connection_nr; ++index) {
            // 保持 gateway_id 不变以复用同一 owner 端口；不同 shard_id 避免注册槽互相替换。
            connections.push_back(
                std::async(std::launch::async, run_connection, endpoint, gateway_id, instance_id, index, sent_nr));
        }

        result total;
        bool failed{};
        for (int index = 0; index < connection_nr; ++index) {
            const auto current = connections[index].get();
            total.sent_bytes += current.sent_bytes;
            total.received_bytes += current.received_bytes;
            total.success += current.success;
            total.rtt_ms += current.rtt_ms;
            if (!current.error.empty()) {
                failed = true;
                std::cerr << "connection=" << index << " FAIL: " << current.error << '\n';
            }
        }
        const auto expected = static_cast<uint64_t>(connection_nr) * sent_nr;
        const double wall_ms = elapsed_ms(begin);
        // 仅统计成功操作的 protobuf 与每消息 5 字节 gRPC 前缀；不含 HTTP/2、TLS 和 TCP 开销。
        const double mib_per_ms = 1000.0 / (1024 * 1024);
        std::print("Message benchmark\n"
                   "  Connections               : {}\n"
                   "  Operations per connection : {}\n"
                   "  Elapsed                   : {:.3f} ms\n"
                   "\nCompleted operations\n"
                   "  Heartbeat                 : {} / {}\n"
                   "\nMean RTT\n"
                   "  Heartbeat                 : {:.3f} ms\n"
                   "\nData transferred\n"
                   "  Sent                      : {} B\n"
                   "  Received                  : {} B\n"
                   "\nThroughput\n"
                   "  Send                      : {:.6f} MiB/s\n"
                   "  Receive                   : {:.6f} MiB/s\n"
                   "  Combined                  : {:.6f} MiB/s\n",
                   connection_nr, sent_nr, wall_ms, total.success, expected,
                   total.success ? total.rtt_ms / total.success : 0, total.sent_bytes, total.received_bytes,
                   wall_ms > 0 ? total.sent_bytes * mib_per_ms / wall_ms : 0,
                   wall_ms > 0 ? total.received_bytes * mib_per_ms / wall_ms : 0,
                   wall_ms > 0 ? (total.sent_bytes + total.received_bytes) * mib_per_ms / wall_ms : 0);

        return failed || total.success != expected ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
