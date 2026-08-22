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

TcpPacket FileService::Upload(uint32_t msgID, int64_t actorUid, TcpPacket& request)
{
    TcpPacket rsp;
    const protocol::FileInfo* requestFile = nullptr;
    int64_t requestOffset = 0;
    std::string requestData;
    bool requestComplete = false;
    if (msgID == ID_FILE_UPLOAD_REQ && request.has_file_upload_request()) {
        const auto& upload = request.file_upload_request();
        requestFile = upload.has_file() ? &upload.file() : nullptr;
        requestOffset = upload.offset();
        requestData = upload.data();
        requestComplete = upload.complete();
        rsp.mutable_file_upload_response();
    } else if (msgID == ID_FILE_UPLOAD_CHUNK_REQ && request.has_file_upload_chunk_request()) {
        const auto& upload = request.file_upload_chunk_request();
        requestFile = upload.has_file() ? &upload.file() : nullptr;
        requestOffset = upload.offset();
        requestData = upload.data();
        requestComplete = upload.complete();
        rsp.mutable_file_upload_chunk_response();
    } else if (msgID == ID_FILE_UPLOAD_COMPLETE_REQ && request.has_file_upload_complete_request()) {
        const auto& upload = request.file_upload_complete_request();
        requestFile = upload.has_file() ? &upload.file() : nullptr;
        requestOffset = upload.offset();
        requestData = upload.data();
        requestComplete = upload.complete();
        rsp.mutable_file_upload_complete_response();
    }
    if (requestFile == nullptr || actorUid <= 0) {
        LOG_WARN(businessLogger, "[message/file-upload] 拒绝缺少文件信息的请求, uid: {}, service_id: {}", actorUid,
                 msgID);
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser, "file metadata is required");
        return rsp;
    }
    LOG_DEBUG(businessLogger,
              "[message/file-upload] 正在处理上传分片, uid: {}, file_id: {}, offset: {}, bytes: {}, complete: {}",
              actorUid, requestFile->file_id(), requestOffset, requestData.size(), requestComplete);

    rpc::UploadRequest rpcRequest;
    rpc::UploadResponse rpcResponse;
    auto* file = rpcRequest.mutable_file();
    file->set_file_id(requestFile->file_id());
    file->set_file_name(requestFile->file_name());
    file->set_file_type(requestFile->file_type());
    file->set_file_size(requestFile->file_size());
    file->set_sha256(requestFile->sha256());

    rpcRequest.set_user_id(actorUid);
    rpcRequest.set_offset(requestOffset);
    rpcRequest.set_data(requestData);
    rpcRequest.set_complete(requestComplete);

    try {
        const auto status = rpc::FileRpc::GetInstance()->forwardUpload(rpcRequest, rpcResponse);
        if (status.ok() && rpcResponse.status() != file::FAILED) {
            const bool complete = rpcResponse.status() == file::SUCCESS && rpcRequest.complete();
            SetTcpPacketStatus(rsp, ErrorCodes::Success);
            if (rsp.has_file_upload_response()) {
                rsp.mutable_file_upload_response()->set_next_offset(rpcResponse.next_offset());
                rsp.mutable_file_upload_response()->set_complete(complete);
            } else if (rsp.has_file_upload_chunk_response()) {
                rsp.mutable_file_upload_chunk_response()->set_next_offset(rpcResponse.next_offset());
                rsp.mutable_file_upload_chunk_response()->set_complete(complete);
            } else {
                rsp.mutable_file_upload_complete_response()->set_next_offset(rpcResponse.next_offset());
                rsp.mutable_file_upload_complete_response()->set_complete(complete);
            }

            const int progressResult = db::MysqlDao::GetInstance()->saveFileUploadProgress(
                actorUid, requestFile->file_id(), rpcResponse.next_offset(), complete, getCurrentDateTime());
            if (progressResult != 0) {
                LOG_ERROR(dbLogger,
                          "[message/file-upload] 保存上传进度失败, uid: {}, file_id: {}, next_offset: {}, result: {}",
                          actorUid, requestFile->file_id(), rpcResponse.next_offset(), progressResult);
                SetTcpPacketStatus(rsp, ErrorCodes::MysqlFailed, "file progress write failed");
                return rsp;
            }

            if (complete) {
                db::FileObjectRecord object;
                object.fileId = requestFile->file_id();
                object.ownerUid = actorUid;
                object.fileName = requestFile->file_name();
                object.fileType = requestFile->file_type();
                object.fileSize = requestFile->file_size();
                object.sha256 = requestFile->sha256();

                const auto now = getCurrentDateTime();
                const int objectResult = db::MysqlDao::GetInstance()->upsertReadyFile(object, now, FileExpiryTime());
                if (objectResult != 0) {
                    LOG_ERROR(dbLogger, "[message/file-upload] 保存文件信息失败, uid: {}, file_id: {}, result: {}",
                              actorUid, requestFile->file_id(), objectResult);
                    SetTcpPacketStatus(rsp, ErrorCodes::MysqlFailed, "file persisted but metadata write failed");
                } else {
                    LOG_INFO(businessLogger, "[message/file-upload] 文件上传完成, uid: {}, file_id: {}, file_size: {}",
                             actorUid, requestFile->file_id(), requestFile->file_size());
                }
            }
        } else {
            LOG_WARN(netLogger, "[message/file-upload] 文件服务拒绝上传, uid: {}, file_id: {}, rpc_status: {}",
                     actorUid, requestFile->file_id(), static_cast<int>(status.error_code()));
            SetTcpPacketStatus(rsp, status.ok() ? ErrorCodes::FileTypeError : ErrorCodes::RPCFailed,
                               status.ok() ? rpcResponse.message() : "RPC failed: " + status.error_message());
        }
    } catch (const std::exception& e) {
        LOG_ERROR(businessLogger, "[message/file-upload] 上传处理异常, uid: {}, file_id: {}, error: {}", actorUid,
                  requestFile->file_id(), e.what());
        SetTcpPacketStatus(rsp, ErrorCodes::InternalError, "系统异常: " + std::string(e.what()));
    }

    return rsp;
}

TcpPacket FileService::Download(uint32_t msgID, int64_t actorUid, TcpPacket& request)
{
    (void)msgID;
    TcpPacket rsp;
    auto* response = rsp.mutable_file_download_response();
    const auto& download = request.file_download_request();
    if (!download.has_file() || actorUid <= 0 || download.file().file_id().empty()) {
        LOG_WARN(businessLogger, "[message/file-download] 拒绝缺少文件信息的请求, uid: {}", actorUid);
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        return rsp;
    }
    const auto fileObject = db::MysqlDao::GetInstance()->findDownloadableFile(download.file().file_id(), actorUid);
    if (!fileObject) {
        LOG_WARN(businessLogger, "[message/file-download] 文件不可用或无权访问, uid: {}, file_id: {}", actorUid,
                 download.file().file_id());
        SetTcpPacketStatus(rsp, ErrorCodes::MessageOwnershipInvalid,
                           "file is unavailable or not owned by this conversation");
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
    rpcRequest.set_offset(download.offset());
    rpcRequest.set_limit(download.limit() > 0 ? download.limit() : 1024 * 1024);

    const auto status = rpc::FileRpc::GetInstance()->forwardDownload(rpcRequest, rpcResponse);
    if (!status.ok() || rpcResponse.status() == file::FAILED) {
        LOG_WARN(netLogger, "[message/file-download] 文件服务读取失败, uid: {}, file_id: {}, rpc_status: {}", actorUid,
                 fileObject->fileId, static_cast<int>(status.error_code()));
        SetTcpPacketStatus(rsp, ErrorCodes::RPCFailed, status.ok() ? rpcResponse.message() : status.error_message());
        return rsp;
    }
    SetTcpPacketStatus(rsp, ErrorCodes::Success);
    response->set_data(rpcResponse.data());
    response->set_next_offset(rpcResponse.next_offset());
    response->set_complete(rpcResponse.complete());

    auto* output = response->mutable_file();
    output->set_file_id(fileObject->fileId);
    output->set_file_name(fileObject->fileName);
    output->set_file_type(fileObject->fileType);
    output->set_file_size(fileObject->fileSize);
    output->set_sha256(fileObject->sha256);
    LOG_DEBUG(businessLogger,
              "[message/file-download] 文件分片读取成功, uid: {}, file_id: {}, offset: {}, bytes: {}, complete: {}",
              actorUid, fileObject->fileId, download.offset(), rpcResponse.data().size(), rpcResponse.complete());
    return rsp;
}

} // namespace wimi
