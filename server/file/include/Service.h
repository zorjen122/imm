#pragma once

#include "file.grpc.pb.h"

#include <atomic>
#include <filesystem>
#include <grpcpp/server_builder.h>
#include <mutex>
#include <string>

namespace wimi::rpc
{

class FileServiceImpl final : public file::FileService::Service
{
  public:
    explicit FileServiceImpl(std::filesystem::path storageRoot);
    grpc::Status Upload(grpc::ServerContext* context, const file::UploadRequest* request,
                        file::UploadResponse* response) override;
    grpc::Status Download(grpc::ServerContext* context, const file::DownloadRequest* request,
                          file::DownloadResponse* response) override;

  private:
    bool IsSafeFileId(const std::string& fileId) const;
    std::filesystem::path ObjectPath(int64_t uid, const std::string& fileId, bool partial) const;
    std::filesystem::path storageRoot_;
    std::mutex mutex_;
};

class FileServer final
{
  public:
    FileServer(size_t workerSize, std::filesystem::path storageRoot);
    ~FileServer();
    bool Run(const std::string& host, unsigned short port);
    void Stop();
    bool stopped() const;

  private:
    std::atomic<bool> stopEnable{true};
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<FileServiceImpl> service;
    std::filesystem::path storageRoot;
};

} // namespace wimi::rpc
