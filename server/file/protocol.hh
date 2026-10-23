#pragma once
#include "file.pb.h"
#include <grpcpp/support/status.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <variant>

namespace imm::file_service
{
inline constexpr int64_t max_file_size = 100LL * 1024 * 1024;
inline constexpr std::size_t max_chunk_size = 1024 * 1024;
using request = std::variant<::file::UploadRequest, ::file::DownloadRequest>;
using response = std::variant<::file::UploadResponse, ::file::DownloadResponse>;
struct result {
    grpc::Status status;
    response payload;
};
bool valid(const ::file::UploadRequest& request);
bool valid(const ::file::DownloadRequest& request);
std::string object_key(int64_t uid, const std::string& id);
unsigned owner_shard(const std::string& key, unsigned shards);
} // namespace imm::file_service
