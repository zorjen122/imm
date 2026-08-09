#include "FileService.h"

#include "Const.h"
#include "FileRpc.h"
#include "Logger.h"
#include "Mysql.h"
#include "Redis.h"

#include <exception>
#include <chrono>
#include <ctime>
#include <grpcpp/support/status.h>
#include <iomanip>
#include <sstream>
#include <string>

namespace wimi
{
namespace
{
std::string FileExpiryTime()
{
    const auto expiry = std::chrono::system_clock::now() + std::chrono::hours(24 * 30);
    const auto time = std::chrono::system_clock::to_time_t(expiry);
    std::tm local{};
    localtime_r(&time, &local);
    std::ostringstream value;
    value << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return value.str();
}

} // namespace

TcpPacket FileService::Upload(uint32_t msgID, TcpPacket& request)
{
    (void)msgID;
    TcpPacket rsp;
    if (!request.has_file() || request.uid() <= 0) {
        rsp.set_error(ErrorCodes::JsonParser);
        rsp.set_message("file metadata is required");
        return rsp;
    }

    rpc::UploadRequest rpcRequest;
    rpc::UploadResponse rpcResponse;
    auto* file = rpcRequest.mutable_file();
    file->set_file_id(request.file().file_id());
    file->set_file_name(request.file().file_name());
    file->set_file_type(request.file().file_type());
    file->set_file_size(request.file().file_size());
    file->set_sha256(request.file().sha256());

    rpcRequest.set_user_id(request.uid());
    rpcRequest.set_offset(request.has_file_offset() ? request.file_offset() : 0);
    rpcRequest.set_data(request.data());
    rpcRequest.set_complete(request.has_file_complete() && request.file_complete());

    try {
        const auto status = rpc::FileRpc::GetInstance()->forwardUpload(rpcRequest, rpcResponse);
        if (status.ok() && rpcResponse.status() != file::FAILED) {
            const bool complete = rpcResponse.status() == file::SUCCESS && rpcRequest.complete();
            rsp.set_error(ErrorCodes::Success);
            rsp.set_file_next_offset(rpcResponse.next_offset());
            rsp.set_file_complete(complete);

            const int progressResult = db::MysqlDao::GetInstance()->saveFileUploadProgress(
                request.uid(),
                request.file().file_id(),
                rpcResponse.next_offset(),
                complete,
                getCurrentDateTime());
            if (progressResult != 0) {
                rsp.set_error(ErrorCodes::MysqlFailed);
                rsp.set_message("file progress write failed");
                return rsp;
            }

            if (complete) {
                db::FileObjectRecord object;
                object.fileId = request.file().file_id();
                object.ownerUid = request.uid();
                object.fileName = request.file().file_name();
                object.fileType = request.file().file_type();
                object.fileSize = request.file().file_size();
                object.sha256 = request.file().sha256();

                const auto now = getCurrentDateTime();
                const int objectResult = db::MysqlDao::GetInstance()->upsertReadyFile(
                    object,
                    now,
                    FileExpiryTime());
                if (objectResult != 0) {
                    rsp.set_error(ErrorCodes::MysqlFailed);
                    rsp.set_message("file persisted but metadata write failed");
                }
            }
        } else {
            rsp.set_error(status.ok() ? ErrorCodes::FileTypeError : ErrorCodes::RPCFailed);
            rsp.set_message(status.ok()
                                ? rpcResponse.message()
                                : "RPC failed: " + status.error_message());
        }
    } catch (const std::exception& e) {
        rsp.set_error(ErrorCodes::InternalError);
        rsp.set_message("系统异常: " + std::string(e.what()));
    }

    return rsp;
}

TcpPacket FileService::Download(uint32_t msgID, TcpPacket& request)
{
    (void)msgID;
    TcpPacket rsp;
    if (!request.has_file() ||
        request.uid() <= 0 ||
        request.file().file_id().empty()) {
        rsp.set_error(ErrorCodes::JsonParser);
        return rsp;
    }
    const auto fileObject = db::MysqlDao::GetInstance()->findDownloadableFile(
        request.file().file_id(),
        request.uid());
    if (!fileObject) {
        rsp.set_error(ErrorCodes::MessageOwnershipInvalid);
        rsp.set_message("file is unavailable or not owned by this conversation");
        return rsp;
    }
    rpc::DownloadRequest rpcRequest;
    rpc::DownloadResponse rpcResponse;
    auto* metadata = rpcRequest.mutable_file();
    metadata->set_file_id(fileObject->fileId);
    metadata->set_file_name(fileObject->fileName);
    metadata->set_file_type(fileObject->fileType);
    metadata->set_file_size(fileObject->fileSize);
    metadata->set_sha256(fileObject->sha256);

    rpcRequest.set_user_id(fileObject->ownerUid);
    rpcRequest.set_offset(request.has_file_offset() ? request.file_offset() : 0);
    rpcRequest.set_limit(request.has_limit() ? request.limit() : 1024 * 1024);

    const auto status = rpc::FileRpc::GetInstance()->forwardDownload(rpcRequest, rpcResponse);
    if (!status.ok() || rpcResponse.status() == file::FAILED) {
        rsp.set_error(ErrorCodes::RPCFailed);
        rsp.set_message(status.ok() ? rpcResponse.message() : status.error_message());
        return rsp;
    }
    rsp.set_error(ErrorCodes::Success);
    rsp.set_data(rpcResponse.data());
    rsp.set_file_next_offset(rpcResponse.next_offset());
    rsp.set_file_complete(rpcResponse.complete());

    auto* output = rsp.mutable_file();
    output->set_file_id(fileObject->fileId);
    output->set_file_name(fileObject->fileName);
    output->set_file_type(fileObject->fileType);
    output->set_file_size(fileObject->fileSize);
    output->set_sha256(fileObject->sha256);
    return rsp;
}

} // namespace wimi
