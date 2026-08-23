#include "Service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <openssl/evp.h>
#include <string_view>

namespace fs = std::filesystem;

namespace wimi::rpc
{
namespace
{
constexpr int64_t kMaxFileSize = 100LL * 1024 * 1024;
constexpr int64_t kMaxChunkSize = 1024LL * 1024;

bool IsDisplayName(const std::string& value, std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum &&
           std::all_of(value.begin(), value.end(),
                       [](unsigned char character) { return character >= 0x20 && character != 0x7f; });
}

bool IsValidUploadMetadata(const file::FileInfo& file)
{
    return IsDisplayName(file.file_name(), 255) && IsDisplayName(file.file_type(), 128) &&
           file.file_type().find('/') != std::string::npos && file.sha256().size() == 32;
}

std::string Sha256File(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!input || !context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return {};
    }

    std::array<char, 64 * 1024> buffer;
    while (input.good()) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (input.gcount() <= 0) {
            continue;
        }
        if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<size_t>(input.gcount())) != 1) {
            return {};
        }
    }

    std::string digest(EVP_MAX_MD_SIZE, '\0');
    unsigned int digestSize = 0;
    if (EVP_DigestFinal_ex(context.get(), reinterpret_cast<unsigned char*>(digest.data()), &digestSize) != 1) {
        return {};
    }
    digest.resize(digestSize);
    return digest;
}

bool ExistingChunkMatches(const fs::path& path, int64_t offset, std::string_view data)
{
    if (data.empty()) {
        return true;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.seekg(offset)) {
        return false;
    }

    std::string existing(data.size(), '\0');
    input.read(existing.data(), static_cast<std::streamsize>(existing.size()));
    return input.gcount() == static_cast<std::streamsize>(data.size()) && existing == data;
}
} // namespace

FileServiceImpl::FileServiceImpl(fs::path storageRoot) : storageRoot_(std::move(storageRoot))
{
    fs::create_directories(storageRoot_);
}

bool FileServiceImpl::IsSafeFileId(const std::string& fileId) const
{
    return fileId.size() == 36 && std::all_of(fileId.begin(), fileId.end(),
                                              [](unsigned char value) { return std::isalnum(value) || value == '-'; });
}

fs::path FileServiceImpl::ObjectPath(int64_t uid, const std::string& fileId, bool partial) const
{
    return storageRoot_ / std::to_string(uid) / (partial ? fileId + ".part" : fileId);
}

grpc::Status FileServiceImpl::Upload(grpc::ServerContext*, const file::UploadRequest* request,
                                     file::UploadResponse* response)
{
    response->set_user_id(request->user_id());
    if (!request->has_file() || request->user_id() <= 0 || !IsSafeFileId(request->file().file_id()) ||
        !IsValidUploadMetadata(request->file()) || request->file().file_size() < 0 ||
        request->file().file_size() > kMaxFileSize || request->offset() < 0 || request->data().size() > kMaxChunkSize) {
        response->set_status(file::FAILED);
        response->set_message("invalid file upload request");
        return grpc::Status::OK;
    }

    std::lock_guard lock(mutex_);
    const auto partial = ObjectPath(request->user_id(), request->file().file_id(), true);
    const auto object = ObjectPath(request->user_id(), request->file().file_id(), false);
    fs::create_directories(partial.parent_path());
    if (fs::exists(object)) {
        const auto completedSize = static_cast<int64_t>(fs::file_size(object));
        if (completedSize != request->file().file_size() || request->file().sha256().size() != 32 ||
            Sha256File(object) != request->file().sha256()) {
            response->set_status(file::FAILED);
            response->set_message("completed file metadata mismatch");
            return grpc::Status::OK;
        }
        response->set_status(file::SUCCESS);
        response->set_next_offset(completedSize);
        return grpc::Status::OK;
    }
    const int64_t current = fs::exists(partial) ? static_cast<int64_t>(fs::file_size(partial)) : 0;
    if (request->offset() != current) {
        if (request->offset() < current) {
            const int64_t storedLength =
                std::min<int64_t>(static_cast<int64_t>(request->data().size()), current - request->offset());
            const std::string_view requestData = request->data();
            const std::string_view storedData = requestData.substr(0, static_cast<std::size_t>(storedLength));
            if (!ExistingChunkMatches(partial, request->offset(), storedData)) {
                response->set_status(file::FAILED);
                response->set_message("retried chunk does not match stored bytes");
                return grpc::Status::OK;
            }
        }

        response->set_status(file::PARTIAL);
        response->set_next_offset(current);
        response->set_message("upload offset differs from stored progress");
        return grpc::Status::OK;
    }
    if (current + request->data().size() > request->file().file_size()) {
        response->set_status(file::FAILED);
        response->set_message("chunk exceeds declared size");
        return grpc::Status::OK;
    }
    if (!request->data().empty()) {
        std::ofstream output(partial, std::ios::binary | std::ios::app);
        output.write(request->data().data(), static_cast<std::streamsize>(request->data().size()));
        if (!output.good()) {
            response->set_status(file::FAILED);
            response->set_message("cannot persist chunk");
            return grpc::Status::OK;
        }
    }
    const int64_t next = current + request->data().size();
    if (request->complete()) {
        if (next != request->file().file_size()) {
            response->set_status(file::PARTIAL);
            response->set_next_offset(next);
            return grpc::Status::OK;
        }
        if (request->file().sha256().size() != 32 || Sha256File(partial) != request->file().sha256()) {
            response->set_status(file::FAILED);
            response->set_message("file sha256 mismatch");
            return grpc::Status::OK;
        }
        fs::rename(partial, object);
    }
    response->set_status(request->complete() ? file::SUCCESS : file::PARTIAL);
    response->set_next_offset(next);
    return grpc::Status::OK;
}

grpc::Status FileServiceImpl::Download(grpc::ServerContext*, const file::DownloadRequest* request,
                                       file::DownloadResponse* response)
{
    if (!request->has_file() || request->user_id() <= 0 || !IsSafeFileId(request->file().file_id()) ||
        request->offset() < 0) {
        response->set_status(file::FAILED);
        response->set_message("invalid file download request");
        return grpc::Status::OK;
    }
    std::lock_guard lock(mutex_);
    const auto object = ObjectPath(request->user_id(), request->file().file_id(), false);
    if (!fs::exists(object)) {
        response->set_status(file::FAILED);
        response->set_message("file unavailable");
        return grpc::Status::OK;
    }
    const int64_t size = static_cast<int64_t>(fs::file_size(object));
    if (request->offset() > size) {
        response->set_status(file::FAILED);
        response->set_message("offset exceeds file size");
        return grpc::Status::OK;
    }
    const int64_t count = std::min<int64_t>(std::max<int64_t>(request->limit(), 1),
                                            std::min<int64_t>(kMaxChunkSize, size - request->offset()));
    std::string data(static_cast<size_t>(count), '\0');
    std::ifstream input(object, std::ios::binary);
    input.seekg(request->offset());
    input.read(data.data(), static_cast<std::streamsize>(count));
    data.resize(static_cast<size_t>(input.gcount()));
    response->set_data(data);
    response->set_next_offset(request->offset() + static_cast<int64_t>(data.size()));
    response->set_complete(response->next_offset() == size);
    response->set_status(file::SUCCESS);
    return grpc::Status::OK;
}

FileServer::FileServer(size_t workers, fs::path root) : storageRoot(std::move(root)) { (void)workers; }

FileServer::~FileServer() { Stop(); }

void FileServer::Run(const std::string& host, unsigned short port)
{
    grpc::ServerBuilder builder;
    builder.AddListeningPort(host + ":" + std::to_string(port), grpc::InsecureServerCredentials());
    builder.SetMaxReceiveMessageSize(1024 * 1024 + 64 * 1024);
    service = std::make_unique<FileServiceImpl>(storageRoot);
    builder.RegisterService(service.get());
    server = builder.BuildAndStart();
    stopEnable = false;
    server->Wait();
}

void FileServer::Stop()
{
    if (server) {
        server->Shutdown();
    }
    stopEnable = true;
}

bool FileServer::stopped() const { return stopEnable.load(); }

} // namespace wimi::rpc
