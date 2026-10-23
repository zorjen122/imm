#pragma once
#include "protocol.hh"
#include <seastar/core/future.hh>
#include <filesystem>
#include <utility>

namespace imm::file_service
{
// 由对象 owner 调用；调用方须将同一 key 的完整请求串行化。函数完成前关闭所有文件句柄。
class storage
{
    std::filesystem::path _root;

  public:
    explicit storage(std::filesystem::path root) : _root(std::move(root)) {}
    seastar::future<> start();
    seastar::future<::file::UploadResponse> upload(::file::UploadRequest request);
    seastar::future<::file::DownloadResponse> download(::file::DownloadRequest request);
};
} // namespace imm::file_service
