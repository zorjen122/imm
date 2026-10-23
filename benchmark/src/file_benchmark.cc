#include "file.grpc.pb.h"
#include <openssl/sha.h>
#include <array>
#include <iomanip>
#include <random>
#include <sstream>
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
    double upload_ms{};
    double download_ms{};
    std::string error;
};

result run_connection(std::string endpoint, int sent_nr, int size)
{
    result result;
    try {
        const std::string data(size, 'x');
        std::array<unsigned char, SHA256_DIGEST_LENGTH> digest;
        if (!SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data())) {
            throw std::runtime_error("SHA256 failed");
        }

        auto stub = file::FileService::NewStub(connect(endpoint));

        for (int index = 0; index < sent_nr; ++index) {
            std::random_device random;
            std::ostringstream id;
            id << std::hex << std::setfill('0');
            for (int index = 0; index < 16; ++index) {
                if (index == 4 || index == 6 || index == 8 || index == 10) {
                    id << '-';
                }
                id << std::setw(2) << (random() & 255);
            }

            file::UploadRequest upload;
            upload.set_user_id(1);
            upload.set_data(data);
            upload.set_complete(true);
            auto* metadata = upload.mutable_file();
            metadata->set_file_id(id.str());
            metadata->set_file_name("benchmark.bin");
            metadata->set_file_type("application/octet-stream");
            metadata->set_file_size(size);
            metadata->set_sha256(digest.data(), digest.size());

            grpc::ClientContext upload_context;
            upload_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
            file::UploadResponse uploaded;
            auto begin = std::chrono::steady_clock::now();
            const auto upload_status = stub->Upload(&upload_context, upload, &uploaded);
            const double upload_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            if (!upload_status.ok() || uploaded.status() != file::SUCCESS || uploaded.next_offset() != size ||
                uploaded.user_id() != 1) {
                throw std::runtime_error("upload failed: " + upload_status.error_message() + " " + uploaded.message());
            }

            file::DownloadRequest download;
            *download.mutable_file() = upload.file();
            download.set_user_id(1);
            download.set_limit(size);

            grpc::ClientContext download_context;
            download_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
            file::DownloadResponse downloaded;
            begin = std::chrono::steady_clock::now();
            const auto download_status = stub->Download(&download_context, download, &downloaded);
            const double download_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            if (!download_status.ok() || downloaded.status() != file::SUCCESS || !downloaded.complete() ||
                downloaded.next_offset() != size || downloaded.data() != data) {
                throw std::runtime_error("download/content check failed: " + download_status.error_message() + " " +
                                         downloaded.message());
            }

            result.sent_bytes += upload.ByteSizeLong() + download.ByteSizeLong() + 10;
            result.received_bytes += uploaded.ByteSizeLong() + downloaded.ByteSizeLong() + 10;

            ++result.success;
            result.upload_ms += upload_ms;
            result.download_ms += download_ms;
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
        std::string endpoint = "127.0.0.1:51100";
        int sent_nr = 100;
        int connection_nr = 1;
        int size = 65536;

        for (int index = 1; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--help") {
                std::cout << "bm_file [--endpoint host:port] [--sent_nr N] [--connection_nr N] [--bytes N]\n";
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
            } else if (option == "--bytes") {
                size = positive_number(value, 1024 * 1024);
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
            connections.push_back(std::async(std::launch::async, run_connection, endpoint, sent_nr, size));
        }

        result total;
        bool failed{};
        for (int index = 0; index < connection_nr; ++index) {
            const auto current = connections[index].get();
            total.sent_bytes += current.sent_bytes;
            total.received_bytes += current.received_bytes;
            total.success += current.success;
            total.upload_ms += current.upload_ms;
            total.download_ms += current.download_ms;
            if (!current.error.empty()) {
                failed = true;
                std::cerr << "connection=" << index << " FAIL: " << current.error << '\n';
            }
        }
        const auto expected = static_cast<uint64_t>(connection_nr) * sent_nr;
        const double wall_ms = elapsed_ms(begin);
        // 仅统计成功操作的 protobuf 与每消息 5 字节 gRPC 前缀；不含 HTTP/2、TLS 和 TCP 开销。
        const double mib_per_ms = 1000.0 / (1024 * 1024);
        std::print("File benchmark\n"
                   "  Connections               : {}\n"
                   "  Operations per connection : {}\n"
                   "  File size                 : {} B\n"
                   "  Elapsed                   : {:.3f} ms\n"
                   "\nCompleted operations\n"
                   "  Upload + download         : {} / {}\n"
                   "\nMean RTT\n"
                   "  Upload                    : {:.3f} ms\n"
                   "  Download                  : {:.3f} ms\n"
                   "\nData transferred\n"
                   "  Sent                      : {} B\n"
                   "  Received                  : {} B\n"
                   "\nThroughput\n"
                   "  Send                      : {:.6f} MiB/s\n"
                   "  Receive                   : {:.6f} MiB/s\n"
                   "  Combined                  : {:.6f} MiB/s\n",
                   connection_nr, sent_nr, size, wall_ms, total.success, expected,
                   total.success ? total.upload_ms / total.success : 0,
                   total.success ? total.download_ms / total.success : 0, total.sent_bytes, total.received_bytes,
                   wall_ms > 0 ? total.sent_bytes * mib_per_ms / wall_ms : 0,
                   wall_ms > 0 ? total.received_bytes * mib_per_ms / wall_ms : 0,
                   wall_ms > 0 ? (total.sent_bytes + total.received_bytes) * mib_per_ms / wall_ms : 0);

        return failed || total.success != expected ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
