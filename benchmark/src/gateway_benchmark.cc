#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/when_all.hh>

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <print>
#include <stdexcept>
#include <string>
#include <map>
#include <vector>

#include "tcp_message.pb.h"

namespace benchmark
{
using namespace seastar;

struct argument_t {
    int connection_nr;
    int sent_nr;
    std::string svr_endpoint;
    int64_t uid;
    std::string token;
};

struct tester {
    struct connection_t {
        connected_socket fd_;
        input_stream<char> in_;
        output_stream<char> out_;
        uint64_t last_sent_bytes{};
        uint64_t last_received_bytes{};

        explicit connection_t(connected_socket socket) : fd_(std::move(socket)), in_(fd_.input()), out_(fd_.output()) {}

        void shutdown()
        {
            fd_.shutdown_input();
            fd_.shutdown_output();
        }
    };

    struct perf_info_t {
        std::vector<double> pkt_rtts;
        uint64_t sent_bytes{};
        uint64_t received_bytes{};
    };

    explicit tester(argument_t arg) : arg_(std::move(arg)) {}

    // 定时器只中断 socket；本协程等待读写退出后才返回，连接始终由调用者持有。
    future<imm::protocol::Packet> exchange(connection_t& connection, imm::protocol::ClientEnvelope request)
    {
        const auto body = request.SerializeAsString();
        const uint32_t length = htonl(static_cast<uint32_t>(body.size()));
        std::string frame(sizeof(length) + body.size(), '\0');
        std::memcpy(frame.data(), &length, sizeof(length));
        std::memcpy(frame.data() + sizeof(length), body.data(), body.size());

        timer<> deadline([&connection] { connection.shutdown(); });
        deadline.arm(std::chrono::seconds(5));
        co_await connection.out_.write(frame);
        co_await connection.out_.flush();

        auto header = co_await connection.in_.read_exactly(sizeof(uint32_t));
        if (header.size() != sizeof(uint32_t))
            throw std::runtime_error("connection closed before response header");

        uint32_t encoded{};
        std::memcpy(&encoded, header.get(), sizeof(encoded));
        const auto bytes = ntohl(encoded);
        if (bytes == 0 || bytes > 10 * 1024 * 1024)
            throw std::runtime_error("invalid response length");

        auto payload = co_await connection.in_.read_exactly(bytes);
        if (payload.size() != bytes)
            throw std::runtime_error("connection closed before response body");

        imm::protocol::ClientEnvelope response;
        if (!response.ParseFromArray(payload.get(), static_cast<int>(payload.size())) || !response.has_response())
            throw std::runtime_error("invalid response envelope");

        const auto& result = response.response();
        if (result.request_id() != request.request().request_id() ||
            result.operation() != request.request().operation())
            throw std::runtime_error("response does not match request");

        if (!result.packet().has_status() || !result.packet().status().has_error() ||
            result.packet().status().error() != 0)
            throw std::runtime_error("request rejected");

        connection.last_sent_bytes = frame.size();
        connection.last_received_bytes = header.size() + payload.size();

        co_return result.packet();
    }

    future<> connect_one(int index)
    {
        auto socket = engine().net().socket();

        // socket connect
        timer<> deadline([&socket] { socket.shutdown(); });
        deadline.arm(std::chrono::seconds(5));
        auto fd = co_await socket.connect(make_ipv4_address(arg_.svr_endpoint));
        deadline.cancel();

        // client verify
        const std::string name = std::to_string(index);

        connections_.try_emplace(name, std::move(fd));
        auto& connection = connections_.find(name)->second;

        imm::protocol::ClientEnvelope login;
        auto* request = login.mutable_request();
        request->set_operation(imm::protocol::OP_LOGIN);
        request->set_request_id("login");
        auto* data = request->mutable_packet()->mutable_login_request();
        data->set_uid(arg_.uid);
        data->set_auth_token(arg_.token);

        // 每条连接使用独立设备身份，避免同一账户的压测连接相互踢出。
        data->set_device_id(_device_prefix + "-" + std::to_string(index));
        auto result = co_await exchange(connection, std::move(login));
        if (!result.has_login_response() || result.login_response().uid() != arg_.uid)
            throw std::runtime_error("invalid login response");
    }

    future<> send_one(std::string name)
    {
        auto& connection = connections_.at(name);
        for (int sequence = 0; sequence < arg_.sent_nr; ++sequence) {
            imm::protocol::ClientEnvelope ping;
            auto* request = ping.mutable_request();
            request->set_operation(imm::protocol::OP_PING);
            request->set_request_id(std::to_string(sequence));
            request->mutable_packet()->mutable_ping_request();

            const auto started = std::chrono::steady_clock::now();
            auto response = co_await exchange(connection, std::move(ping));
            if (!response.has_ping_response()) {
                throw std::runtime_error("invalid ping response");
            }

            perfs_[name].sent_bytes += connection.last_sent_bytes;
            perfs_[name].received_bytes += connection.last_received_bytes;

            const auto end = std::chrono::steady_clock::now();
            perfs_[name].pkt_rtts.push_back(std::chrono::duration<double, std::milli>(end - started).count());
        }
    }

    future<bool> start()
    {
        connections_.erase(connections_.begin(), connections_.end());
        perfs_.erase(perfs_.begin(), perfs_.end());

        std::exception_ptr failure;
        std::vector<future<>> futures;
        std::chrono::steady_clock::time_point io_begin;
        double io_ms{};
        bool measuring{};
        try {

            // prepare
            for (int index = 0; index < arg_.connection_nr; ++index)
                futures.push_back(connect_one(index));
            co_await when_all_succeed(futures.begin(), futures.end());

            futures.clear();

            io_begin = std::chrono::steady_clock::now();
            measuring = true;

            // data exchange
            for (const auto& [name, connection] : connections_)
                futures.push_back(send_one(name));
            co_await when_all_succeed(futures.begin(), futures.end());

        } catch (...) {
            failure = std::current_exception();
        }

        if (measuring) {
            io_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - io_begin).count();
        }

        // connections clear
        futures.clear();
        for (auto& [name, connection] : connections_) {
            connection.shutdown();
            futures.push_back(connection.in_.close());
            futures.push_back(connection.out_.close());
        }
        auto closed = co_await when_all(futures.begin(), futures.end());

        for (auto& result : closed) {
            if (result.failed()) {
                auto error = result.get_exception();

                if (!failure)
                    failure = error;
            }
        }

        size_t samples = 0;
        double total_ms = 0;
        uint64_t sent_bytes{};
        uint64_t received_bytes{};
        for (const auto& [name, perf] : perfs_) {
            sent_bytes += perf.sent_bytes;
            received_bytes += perf.received_bytes;
            for (double rtt : perf.pkt_rtts) {
                ++samples;
                total_ms += rtt;
            }
        }
        // PING 阶段的应用层帧字节数包含 4 字节长度前缀，登录与关闭不计入测量。
        const double mib_per_ms = 1000.0 / (1024 * 1024);
        std::print("Gateway benchmark\n"
                   "  Connections               : {}\n"
                   "  Operations per connection : {}\n"
                   "  Elapsed                   : {:.3f} ms\n"
                   "\nCompleted operations\n"
                   "  PING                      : {} / {}\n"
                   "\nMean RTT\n"
                   "  PING                      : {:.3f} ms\n"
                   "\nData transferred\n"
                   "  Sent                      : {} B\n"
                   "  Received                  : {} B\n"
                   "\nThroughput\n"
                   "  Send                      : {:.6f} MiB/s\n"
                   "  Receive                   : {:.6f} MiB/s\n"
                   "  Combined                  : {:.6f} MiB/s\n",
                   arg_.connection_nr, arg_.sent_nr, io_ms, samples,
                   static_cast<size_t>(arg_.connection_nr) * arg_.sent_nr, samples == 0 ? 0.0 : total_ms / samples,
                   sent_bytes, received_bytes, io_ms > 0 ? sent_bytes * mib_per_ms / io_ms : 0,
                   io_ms > 0 ? received_bytes * mib_per_ms / io_ms : 0,
                   io_ms > 0 ? (sent_bytes + received_bytes) * mib_per_ms / io_ms : 0);

        if (failure) {
            std::rethrow_exception(failure);
        }

        co_return true;
    }

    std::map<std::string, connection_t> connections_;
    std::map<std::string, perf_info_t> perfs_;
    argument_t arg_;
    std::string _device_prefix = "bench-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
};

// tester 在协程帧中持有，直到全部连接任务完成并关闭后才销毁。
seastar::future<int> run(argument_t arg)
{
    tester test(std::move(arg));
    try {
        co_return co_await test.start() ? 0 : 1;
    } catch (const std::exception& error) {
        std::print("io failed, err: {}\n", error.what());
        co_return 1;
    }
}
} // namespace benchmark

int main(int argc, char* argv[])
{
    namespace po = boost::program_options;
    seastar::app_template app;
    auto arguments = app.add_options();
    arguments("endpoint", po::value<std::string>()->default_value("127.0.0.1:8090"), "Gateway IPv4 address:port");
    arguments("uid", po::value<int64_t>()->required(), "User ID for IMM_BENCH_TOKEN");
    arguments("connections", po::value<int>()->default_value(1), "Number of connections");
    arguments("sent", po::value<int>()->default_value(1), "Pings per connection");

    return app.run(argc, argv, [&app]() -> seastar::future<int> {
        const auto& config = app.configuration();
        const char* token = std::getenv("IMM_BENCH_TOKEN");
        const int connections = config["connections"].as<int>();
        const auto uid = config["uid"].as<int64_t>();
        const int sent_nr = config["sent"].as<int>();
        if (!token || !*token || connections <= 0 || uid <= 0 || sent_nr <= 0) {
            std::print("Bad arguments\n");
            return seastar::make_ready_future<int>(1);
        }
        return benchmark::run({connections, sent_nr, config["endpoint"].as<std::string>(), uid, token});
    });
}
