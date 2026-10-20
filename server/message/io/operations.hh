#pragma once
#include "io/mysql_operations.hh"
#include "file.pb.h"
#include "gateway_message.pb.h"

namespace wimi::message_io
{
struct get_session {
    using result_type = gateway::SessionLease;
    int64_t uid{};
    std::string device_id;
    size_t byte_size() const { return sizeof(*this) + device_id.size(); }
};
struct get_sessions {
    using result_type = std::vector<gateway::SessionLease>;
    int64_t uid{};
    size_t byte_size() const { return sizeof(*this); }
};
enum class id_kind { message, conversation, friendship, group };
struct generate_id {
    using result_type = int64_t;
    id_kind kind;
    size_t byte_size() const { return sizeof(*this); }
};
struct upload_file {
    using result_type = file::UploadResponse;
    file::UploadRequest request;
    size_t byte_size() const { return sizeof(*this) + request.ByteSizeLong(); }
};
struct download_file {
    using result_type = file::DownloadResponse;
    file::DownloadRequest request;
    size_t byte_size() const { return sizeof(*this) + request.ByteSizeLong(); }
};
// 将 SQL、Redis 与 File RPC 注册到同一入口；结果类型随操作确定。
// 新操作只扩展协议边界和本注册表，不在业务分派处追加依赖种类判断。
template <typename> struct registered_operations;
template <typename... Sql> struct registered_operations<std::variant<Sql...>> {
    using type = std::variant<Sql..., get_session, get_sessions, generate_id, upload_file, download_file>;
};
using operations = registered_operations<mysql_operations>::type;
} // namespace wimi::message_io
