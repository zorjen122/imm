#include "file.grpc.pb.h"
#include "state.grpc.pb.h"
#include "tcp_message.pb.h"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <grpcpp/grpcpp.h>
#include <jsoncpp/json/json.h>
#include <openssl/sha.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <algorithm>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace asio = boost::asio;
namespace http = boost::beast::http;
namespace protocol = imm::protocol;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string Env(const char* name, const char* value)
{
    const auto* found = std::getenv(name);
    return found ? found : value;
}

struct Sample {
    uint64_t sequence;
    double scheduled, sent, completed;
    bool success;
    std::string error;
};

class Client
{
  public:
    Client(std::string scenario, int worker)
        : scenario(std::move(scenario)), worker(worker), tcpSocket(io),
          state(state::StateService::NewStub(
              grpc::CreateChannel(Env("PERF_STATE", "127.0.0.1:50052"), grpc::InsecureChannelCredentials()))),
          file(file::FileService::NewStub(
              grpc::CreateChannel(Env("PERF_FILE", "127.0.0.1:51100"), grpc::InsecureChannelCredentials())))
    {
        if (this->scenario.ends_with("-keepalive")) {
            httpConnection = std::make_unique<boost::beast::tcp_stream>(io);
            asio::connect(httpConnection->socket(),
                          asio::ip::tcp::resolver(io).resolve("127.0.0.1", Env("PERF_GATE_PORT", "18080")));
            httpConnection->expires_after(3s);
        }
        if (this->scenario.starts_with("tcp-")) {
            auto auth = Login();
            asio::connect(tcpSocket.socket(),
                          asio::ip::tcp::resolver(io).resolve("127.0.0.1", Env("PERF_GATEWAY_PORT", "8090")));
            tcpSocket.expires_after(3s);
            protocol::Packet packet;
            auto* login = packet.mutable_login_request();
            login->set_uid(auth["uid"].asInt64());
            login->set_auth_token(auth["chatToken"].asString());
            login->set_device_id("perf-" + std::to_string(worker));
            Exchange(protocol::OP_LOGIN, std::move(packet), "login");
        }
    }

    void Run(uint64_t sequence)
    {
        if (scenario == "http-test" || scenario == "http-test-keepalive") {
            if (Http("/test", "") != "[TEST]")
                throw std::runtime_error("incorrect HTTP payload");
        } else if (scenario == "http-login" || scenario == "http-login-keepalive") {
            Login();
        } else if (scenario == "state-pick") {
            state::ConnectUser request;
            state::ConnectUserRsp response;
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 3s);
            auto status = state->PickConnectionGateway(&context, request, &response);
            if (!status.ok() || response.node_id().empty() || response.port() <= 0)
                throw std::runtime_error("state pick failed");
        } else if (scenario == "state-topology") {
            state::TopologyRequest request;
            state::MessageTopology response;
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 3s);
            if (!state->ListMessageNodes(&context, request, &response).ok() || response.nodes_size() != 2)
                throw std::runtime_error("state topology mismatch");
        } else if (scenario == "file-roundtrip") {
            FileRoundtrip(sequence);
        } else {
            protocol::Packet packet;
            const bool ping = scenario == "tcp-ping";
            if (ping)
                packet.mutable_ping_request();
            else
                packet.mutable_pull_friend_list_request();
            auto response = Exchange(ping ? protocol::OP_PING : protocol::OP_PULL_FRIEND_LIST, std::move(packet),
                                     std::to_string(sequence));
            if (ping ? !response.has_ping_response() : !response.has_pull_friend_list_response())
                throw std::runtime_error("incorrect response payload");
        }
    }

  private:
    template <typename Start> void Wait(Start start)
    {
        io.restart();
        auto result = start();
        io.run();
        result.get();
    }

    std::string Http(const std::string& path, const std::string& body)
    {
        if (httpConnection)
            return HttpOn(*httpConnection, path, body, true);

        boost::beast::tcp_stream connection(io);
        asio::connect(connection.socket(),
                      asio::ip::tcp::resolver(io).resolve("127.0.0.1", Env("PERF_GATE_PORT", "18080")));
        connection.expires_after(3s);
        return HttpOn(connection, path, body, false);
    }

    std::string HttpOn(boost::beast::tcp_stream& connection, const std::string& path, const std::string& body,
                       bool keepAlive)
    {
        connection.expires_after(3s);
        http::request<http::string_body> request(body.empty() ? http::verb::get : http::verb::post, path, 11);
        request.set(http::field::host, "localhost");
        request.set(http::field::content_type, "application/json");
        request.keep_alive(keepAlive);
        request.body() = body;
        request.prepare_payload();
        Wait([&] { return http::async_write(connection, request, asio::use_future); });
        http::response_parser<http::string_body> parser;
        parser.body_limit(1024 * 1024);
        Wait([&] { return http::async_read(connection, httpBuffer, parser, asio::use_future); });
        auto response = parser.release();
        if (response.result() != http::status::ok)
            throw std::runtime_error("HTTP status " + std::to_string(response.result_int()));
        if (keepAlive && !response.keep_alive())
            throw std::runtime_error("server closed HTTP keep-alive connection");
        return std::move(response.body());
    }

    Json::Value Login()
    {
        Json::Value request;
        request["username"] = "perf" + std::to_string(worker);
        request["password"] = "perf-test-only";
        auto body = Http("/post-signIn", Json::writeString(Json::StreamWriterBuilder(), request));
        Json::Value response;
        Json::Reader reader;
        if (!reader.parse(body, response) || !response.isMember("error") || response["error"].asInt() != 0 ||
            response["chatToken"].asString().empty() || response["uid"].asInt64() <= 0)
            throw std::runtime_error("login business response failed");
        return response;
    }

    protocol::Packet Exchange(protocol::Operation operation, protocol::Packet packet, const std::string& id)
    {
        protocol::ClientEnvelope envelope;
        auto* request = envelope.mutable_request();
        request->set_request_id(id);
        request->set_operation(operation);
        *request->mutable_packet() = std::move(packet);
        auto payload = envelope.SerializeAsString();
        uint32_t size = htonl(static_cast<uint32_t>(payload.size()));
        std::array<asio::const_buffer, 2> buffers{asio::buffer(&size, sizeof(size)), asio::buffer(payload)};
        tcpSocket.expires_after(3s);
        Wait([&] { return asio::async_write(tcpSocket, buffers, asio::use_future); });
        Wait([&] { return asio::async_read(tcpSocket, asio::buffer(&size, sizeof(size)), asio::use_future); });
        size = ntohl(size);
        if (size > 10 * 1024 * 1024)
            throw std::runtime_error("oversized response");
        payload.resize(size);
        Wait([&] { return asio::async_read(tcpSocket, asio::buffer(payload), asio::use_future); });
        if (!envelope.ParseFromString(payload) || !envelope.has_response() || envelope.response().request_id() != id ||
            envelope.response().operation() != operation || !envelope.response().packet().has_status() ||
            envelope.response().packet().status().error() != 0)
            throw std::runtime_error("request correlation or business response failed");
        return std::move(*envelope.mutable_response()->mutable_packet());
    }

    void FileRoundtrip(uint64_t sequence)
    {
        const std::string data(64 * 1024, static_cast<char>('a' + sequence % 26));
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest);
        file::UploadRequest request;
        request.set_user_id(100000 + worker);
        request.set_data(data);
        request.set_complete(true);
        auto* info = request.mutable_file();
        char id[37];
        std::snprintf(id, sizeof(id), "%08x-%04x-4000-8000-%012llx", static_cast<unsigned>(::getpid()), worker,
                      static_cast<unsigned long long>(sequence));
        info->set_file_id(id);
        info->set_file_name("perf.bin");
        info->set_file_type("application/octet-stream");
        info->set_file_size(data.size());
        info->set_sha256(digest, sizeof(digest));
        grpc::ClientContext uploadContext;
        uploadContext.set_deadline(std::chrono::system_clock::now() + 3s);
        file::UploadResponse uploaded;
        if (!file->Upload(&uploadContext, request, &uploaded).ok() || uploaded.status() != file::SUCCESS ||
            uploaded.next_offset() != static_cast<int64_t>(data.size()))
            throw std::runtime_error("file upload failed");
        file::DownloadRequest download;
        *download.mutable_file() = *info;
        download.set_user_id(request.user_id());
        download.set_limit(data.size());
        grpc::ClientContext downloadContext;
        downloadContext.set_deadline(std::chrono::system_clock::now() + 3s);
        file::DownloadResponse downloaded;
        if (!file->Download(&downloadContext, download, &downloaded).ok() || downloaded.status() != file::SUCCESS ||
            !downloaded.complete() || downloaded.data() != data)
            throw std::runtime_error("file download content mismatch");
    }

    std::string scenario;
    int worker;
    asio::io_context io;
    boost::beast::tcp_stream tcpSocket;
    std::unique_ptr<boost::beast::tcp_stream> httpConnection;
    boost::beast::flat_buffer httpBuffer;
    std::unique_ptr<state::StateService::Stub> state;
    std::unique_ptr<file::FileService::Stub> file;
};

int main(int argc, char** argv)
{
    try {
        if (argc != 6)
            throw std::runtime_error("Usage: immPerf SCENARIO RATE SECONDS WORKERS OUTPUT.jsonl");
        const std::string scenario = argv[1];
        const std::vector<std::string> supported{
            "http-test",     "http-test-keepalive", "http-login", "http-login-keepalive",
            "state-pick",    "state-topology",      "tcp-ping",   "tcp-pull",
            "file-roundtrip"};
        const double rate = std::stod(argv[2]);
        const double duration = std::stod(argv[3]);
        const int workers = std::stoi(argv[4]);
        if (std::ranges::find(supported, scenario) == supported.end() || !std::isfinite(rate) ||
            !std::isfinite(duration) || rate <= 0 || duration <= 0 || workers < 1 || workers > 256 ||
            rate * duration > 10000000)
            throw std::runtime_error("invalid scenario or load bounds (maximum 10 million samples)");
        const uint64_t planned = static_cast<uint64_t>(std::ceil(rate * duration));
        std::ofstream output(argv[5]);
        if (!output)
            throw std::runtime_error("cannot create output");

        std::vector<std::unique_ptr<Client>> clients;
        for (int worker = 0; worker < workers; ++worker)
            clients.push_back(std::make_unique<Client>(scenario, worker));
        std::vector<std::vector<Sample>> samples(workers);
        std::barrier startBarrier(workers + 1);
        Clock::time_point start;
        std::vector<std::jthread> threads;
        for (int worker = 0; worker < workers; ++worker) {
            samples[worker].reserve(planned / workers + 1);
            threads.emplace_back([&, worker] {
                startBarrier.arrive_and_wait();
                for (uint64_t index = worker; index < planned; index += workers) {
                    const auto scheduled = start + std::chrono::duration_cast<Clock::duration>(
                                                       std::chrono::duration<double>(index / rate));
                    std::this_thread::sleep_until(scheduled);
                    auto sent = Clock::now();
                    Sample sample{index, index / rate, std::chrono::duration<double>(sent - start).count(),
                                  0,     false,        {}};
                    // Missed slots are counted, never replayed in a catch-up burst.
                    if (sent - scheduled >= std::chrono::duration<double>(workers / rate)) {
                        sample.error = "generator_missed_slot";
                    } else {
                        try {
                            clients[worker]->Run(index);
                            sample.success = true;
                        } catch (const std::exception& error) {
                            sample.error = error.what();
                        }
                    }
                    sample.completed = std::chrono::duration<double>(Clock::now() - start).count();
                    samples[worker].push_back(std::move(sample));
                }
            });
        }
        start = Clock::now() + 100ms;
        startBarrier.arrive_and_wait();
        threads.clear();

        uint64_t success = 0, completedInWindow = 0, missed = 0;
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        for (const auto& batch : samples) {
            for (const auto& sample : batch) {
                Json::Value row;
                row["sequence"] = Json::UInt64(sample.sequence);
                row["scheduled_s"] = sample.scheduled;
                row["sent_s"] = sample.sent;
                row["completed_s"] = sample.completed;
                row["success"] = sample.success;
                row["error"] = sample.error;
                output << Json::writeString(writer, row) << '\n';
                success += sample.success;
                completedInWindow += sample.success && sample.completed < duration;
                missed += sample.error == "generator_missed_slot";
            }
        }
        output.close();
        if (!output)
            throw std::runtime_error("output write failed");
        Json::Value summary;
        summary["scenario"] = scenario;
        summary["planned"] = Json::UInt64(planned);
        summary["success"] = Json::UInt64(success);
        summary["generator_missed"] = Json::UInt64(missed);
        summary["duration_s"] = duration;
        summary["success_tps_in_window"] = completedInWindow / duration;
        std::cout << Json::writeString(writer, summary) << '\n';
        return success == planned ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
