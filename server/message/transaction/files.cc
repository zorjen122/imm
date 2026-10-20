#include "transaction/files.hh"

#include "Const.h"

#include "Logger.h"

#include <exception>
#include <chrono>
#include <ctime>
#include <grpcpp/support/status.h>
#include <iomanip>
#include <sstream>
#include <string>

namespace imm::message_transaction
{
namespace
{
std::string file_expiry_time()
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

seastar::future<protocol::Packet> files::upload(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    const auto& request = context.packet;
    TcpPacket rsp;
    const protocol::FileInfo* request_file = nullptr;
    int64_t request_offset = 0;
    std::string request_data;
    bool request_complete = false;
    if (request.has_file_upload_request()) {
        const auto& upload = request.file_upload_request();
        request_file = upload.has_file() ? &upload.file() : nullptr;
        request_offset = upload.offset();
        request_data = upload.data();
        request_complete = upload.complete();
        rsp.mutable_file_upload_response();
    } else if (request.has_file_upload_chunk_request()) {
        const auto& upload = request.file_upload_chunk_request();
        request_file = upload.has_file() ? &upload.file() : nullptr;
        request_offset = upload.offset();
        request_data = upload.data();
        request_complete = upload.complete();
        rsp.mutable_file_upload_chunk_response();
    } else if (request.has_file_upload_complete_request()) {
        const auto& upload = request.file_upload_complete_request();
        request_file = upload.has_file() ? &upload.file() : nullptr;
        request_offset = upload.offset();
        request_data = upload.data();
        request_complete = upload.complete();
        rsp.mutable_file_upload_complete_response();
    }
    if (request_file == nullptr || actor_uid <= 0) {
        LOG_WARN(logger, "[message/file-upload] 拒绝缺少文件信息的请求, uid: {}", actor_uid);
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser, "file metadata is required");
        co_return rsp;
    }
    LOG_DEBUG(logger,
              "[message/file-upload] 正在处理上传分片, uid: {}, file_id: {}, offset: {}, bytes: {}, complete: {}",
              actor_uid, request_file->file_id(), request_offset, request_data.size(), request_complete);

    file::UploadRequest rpc_request;
    auto* file = rpc_request.mutable_file();
    file->set_file_id(request_file->file_id());
    file->set_file_name(request_file->file_name());
    file->set_file_type(request_file->file_type());
    file->set_file_size(request_file->file_size());
    file->set_sha256(request_file->sha256());

    rpc_request.set_user_id(actor_uid);
    rpc_request.set_offset(request_offset);
    rpc_request.set_data(request_data);
    rpc_request.set_complete(request_complete);

    message_io::upload_file upload_operation{rpc_request};
    auto rpc_response = co_await _io.submit(std::move(upload_operation));
    if (rpc_response.status() != file::FAILED) {
        const bool complete = rpc_response.status() == file::SUCCESS && rpc_request.complete();
        SetTcpPacketStatus(rsp, ErrorCodes::Success);
        if (rsp.has_file_upload_response()) {
            rsp.mutable_file_upload_response()->set_next_offset(rpc_response.next_offset());
            rsp.mutable_file_upload_response()->set_complete(complete);
        } else if (rsp.has_file_upload_chunk_response()) {
            rsp.mutable_file_upload_chunk_response()->set_next_offset(rpc_response.next_offset());
            rsp.mutable_file_upload_chunk_response()->set_complete(complete);
        } else {
            rsp.mutable_file_upload_complete_response()->set_next_offset(rpc_response.next_offset());
            rsp.mutable_file_upload_complete_response()->set_complete(complete);
        }

        message_io::save_file_upload_progress save_file_upload_progress_operation{
            actor_uid, request_file->file_id(), rpc_response.next_offset(), complete, getCurrentDateTime()};
        const int progress_result = co_await _io.submit(std::move(save_file_upload_progress_operation));
        if (progress_result != 0) {
            LOG_ERROR(logger,
                      "[message/file-upload] 保存上传进度失败, uid: {}, file_id: {}, next_offset: {}, result: {}",
                      actor_uid, request_file->file_id(), rpc_response.next_offset(), progress_result);
            SetTcpPacketStatus(rsp, ErrorCodes::MysqlFailed, "file progress write failed");
            co_return rsp;
        }

        if (complete) {
            db::FileObjectRecord object;
            object.fileId = request_file->file_id();
            object.ownerUid = actor_uid;
            object.fileName = request_file->file_name();
            object.fileType = request_file->file_type();
            object.fileSize = request_file->file_size();
            object.sha256 = request_file->sha256();

            const auto now = getCurrentDateTime();
            message_io::upsert_ready_file upsert_ready_file_operation{object, now, file_expiry_time()};
            const int object_result = co_await _io.submit(std::move(upsert_ready_file_operation));
            if (object_result != 0) {
                LOG_ERROR(logger, "[message/file-upload] 保存文件信息失败, uid: {}, file_id: {}, result: {}", actor_uid,
                          request_file->file_id(), object_result);
                SetTcpPacketStatus(rsp, ErrorCodes::MysqlFailed, "file persisted but metadata write failed");
            } else {
                LOG_INFO(logger, "[message/file-upload] 文件上传完成, uid: {}, file_id: {}, file_size: {}", actor_uid,
                         request_file->file_id(), request_file->file_size());
            }
        }
    } else {
        LOG_WARN(logger, "[message/file-upload] 文件服务拒绝上传, uid: {}, file_id: {}, rpc_status: {}", actor_uid,
                 request_file->file_id(), static_cast<int>(rpc_response.status()));
        SetTcpPacketStatus(rsp, ErrorCodes::FileTypeError, rpc_response.message());
    }

    co_return rsp;
}

seastar::future<protocol::Packet> files::download(request_context& context)
{
    const auto actor_uid = context.command.actor_uid();
    const auto& request = context.packet;
    TcpPacket rsp;
    auto* response = rsp.mutable_file_download_response();
    const auto& download = request.file_download_request();
    if (!download.has_file() || actor_uid <= 0 || download.file().file_id().empty()) {
        LOG_WARN(logger, "[message/file-download] 拒绝缺少文件信息的请求, uid: {}", actor_uid);
        SetTcpPacketStatus(rsp, ErrorCodes::JsonParser);
        co_return rsp;
    }
    message_io::find_downloadable_file find_downloadable_file_operation{download.file().file_id(), actor_uid};
    const auto file_object = co_await _io.submit(std::move(find_downloadable_file_operation));
    if (!file_object) {
        LOG_WARN(logger, "[message/file-download] 文件不可用或无权访问, uid: {}, file_id: {}", actor_uid,
                 download.file().file_id());
        SetTcpPacketStatus(rsp, ErrorCodes::MessageOwnershipInvalid,
                           "file is unavailable or not owned by this conversation");
        co_return rsp;
    }
    file::DownloadRequest rpc_request;
    auto* metadata = rpc_request.mutable_file();
    metadata->set_file_id(file_object->fileId);
    metadata->set_file_name(file_object->fileName);
    metadata->set_file_type(file_object->fileType);
    metadata->set_file_size(file_object->fileSize);
    metadata->set_sha256(file_object->sha256);

    rpc_request.set_user_id(file_object->ownerUid);
    rpc_request.set_offset(download.offset());
    rpc_request.set_limit(download.limit() > 0 ? download.limit() : 1024 * 1024);

    message_io::download_file download_operation{rpc_request};
    auto rpc_response = co_await _io.submit(std::move(download_operation));
    if (rpc_response.status() == file::FAILED) {
        LOG_WARN(logger, "[message/file-download] 文件服务读取失败, uid: {}, file_id: {}, rpc_status: {}", actor_uid,
                 file_object->fileId, static_cast<int>(rpc_response.status()));
        SetTcpPacketStatus(rsp, ErrorCodes::RPCFailed, rpc_response.message());
        co_return rsp;
    }
    SetTcpPacketStatus(rsp, ErrorCodes::Success);
    response->set_data(rpc_response.data());
    response->set_next_offset(rpc_response.next_offset());
    response->set_complete(rpc_response.complete());

    auto* output = response->mutable_file();
    output->set_file_id(file_object->fileId);
    output->set_file_name(file_object->fileName);
    output->set_file_type(file_object->fileType);
    output->set_file_size(file_object->fileSize);
    output->set_sha256(file_object->sha256);
    LOG_DEBUG(logger,
              "[message/file-download] 文件分片读取成功, uid: {}, file_id: {}, offset: {}, bytes: {}, complete: {}",
              actor_uid, file_object->fileId, download.offset(), rpc_response.data().size(), rpc_response.complete());
    co_return rsp;
}

} // namespace imm::message_transaction
