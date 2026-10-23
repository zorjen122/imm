#include "state.grpc.pb.h"
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
    uint64_t pick_success{};
    uint64_t topology_success{};
    double pick_ms{};
    double topology_ms{};
    std::string error;
};

result run_connection(std::string endpoint, int sent_nr)
{
    result result;
    try {
        auto stub = state::StateService::NewStub(connect(endpoint));
        for (bool topology : {false, true}) {
            for (int index = 0; index < sent_nr; ++index) {
                grpc::ClientContext context;
                context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
                const auto begin = std::chrono::steady_clock::now();
                grpc::Status status;
                uint64_t sent_bytes{};
                uint64_t received_bytes{};
                if (topology) {
                    state::TopologyRequest request;
                    state::MessageTopology response;
                    status = stub->ListMessageNodes(&context, request, &response);
                    sent_bytes = request.ByteSizeLong() + 5;
                    received_bytes = response.ByteSizeLong() + 5;
                    if (status.ok() && response.nodes().empty()) {
                        throw std::runtime_error("ListMessageNodes returned no nodes");
                    }
                } else {
                    state::ConnectUser request;
                    request.set_id(index + 1);
                    state::ConnectUserRsp response;
                    status = stub->PickConnectionGateway(&context, request, &response);
                    sent_bytes = request.ByteSizeLong() + 5;
                    received_bytes = response.ByteSizeLong() + 5;
                    if (status.ok() && (response.ip().empty() || response.port() <= 0 || response.node_id().empty())) {
                        throw std::runtime_error("PickConnectionGateway returned an invalid node");
                    }
                }

                if (!status.ok()) {
                    throw std::runtime_error(status.error_message());
                }

                result.sent_bytes += sent_bytes;
                result.received_bytes += received_bytes;

                if (topology) {
                    ++result.topology_success;
                    result.topology_ms += elapsed_ms(begin);
                } else {
                    ++result.pick_success;
                    result.pick_ms += elapsed_ms(begin);
                }
            }
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
        std::string endpoint = "127.0.0.1:50052";
        int sent_nr = 100;
        int connection_nr = 1;

        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--help") {
                std::cout << "bm_state [--endpoint host:port] [--sent_nr N] [--connection_nr N]\n";
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
            } else {
                throw std::invalid_argument("unknown option: " + option);
            }
        }

        if (endpoint.empty()) {
            throw std::invalid_argument("endpoint must be nonempty");
        }

        std::vector<std::future<result>> connections;
        const auto begin = std::chrono::steady_clock::now();
        for (int index = 0; index < connection_nr; ++index) {
            connections.push_back(std::async(std::launch::async, run_connection, endpoint, sent_nr));
        }

        result total;
        bool failed{};
        for (int index = 0; index < connection_nr; ++index) {
            const auto current = connections[index].get();
            total.sent_bytes += current.sent_bytes;
            total.received_bytes += current.received_bytes;
            total.pick_success += current.pick_success;
            total.topology_success += current.topology_success;
            total.pick_ms += current.pick_ms;
            total.topology_ms += current.topology_ms;
            if (!current.error.empty()) {
                failed = true;
                std::cerr << "connection=" << index << " FAIL: " << current.error << '\n';
            }
        }

        const auto expected = static_cast<uint64_t>(connection_nr) * sent_nr;
        const double wall_ms = elapsed_ms(begin);
        // 仅统计成功操作的 protobuf 与每消息 5 字节 gRPC 前缀；不含 HTTP/2、TLS 和 TCP 开销。
        const double mib_per_ms = 1000.0 / (1024 * 1024);
        std::print("State benchmark\n"
                   "  Connections               : {}\n"
                   "  Operations per connection : {} per RPC\n"
                   "  Elapsed                   : {:.3f} ms\n"
                   "\nCompleted operations\n"
                   "  PickConnectionGateway     : {} / {}\n"
                   "  ListMessageNodes          : {} / {}\n"
                   "\nMean RTT\n"
                   "  PickConnectionGateway     : {:.3f} ms\n"
                   "  ListMessageNodes          : {:.3f} ms\n"
                   "\nData transferred\n"
                   "  Sent                      : {} B\n"
                   "  Received                  : {} B\n"
                   "\nThroughput\n"
                   "  Send                      : {:.6f} MiB/s\n"
                   "  Receive                   : {:.6f} MiB/s\n"
                   "  Combined                  : {:.6f} MiB/s\n",
                   connection_nr, sent_nr, wall_ms, total.pick_success, expected, total.topology_success, expected,
                   total.pick_success ? total.pick_ms / total.pick_success : 0,
                   total.topology_success ? total.topology_ms / total.topology_success : 0, total.sent_bytes,
                   total.received_bytes, wall_ms > 0 ? total.sent_bytes * mib_per_ms / wall_ms : 0,
                   wall_ms > 0 ? total.received_bytes * mib_per_ms / wall_ms : 0,
                   wall_ms > 0 ? (total.sent_bytes + total.received_bytes) * mib_per_ms / wall_ms : 0);

        return failed || total.pick_success != expected || total.topology_success != expected ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
