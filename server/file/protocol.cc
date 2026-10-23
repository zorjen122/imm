#include "protocol.hh"
#include <algorithm>
#include <cctype>
#include <type_traits>

namespace imm::file_service
{
namespace
{
bool safe_id(const std::string& id)
{
    return id.size() == 36 &&
           std::all_of(id.begin(), id.end(), [](unsigned char value) { return std::isalnum(value) || value == '-'; });
}
bool display_name(const std::string& value, std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum &&
           std::all_of(value.begin(), value.end(), [](unsigned char value) { return value >= 0x20 && value != 0x7f; });
}
} // namespace
bool valid(const ::file::UploadRequest& request)
{
    return request.has_file() && request.user_id() > 0 && safe_id(request.file().file_id()) &&
           display_name(request.file().file_name(), 255) && display_name(request.file().file_type(), 128) &&
           request.file().file_type().find('/') != std::string::npos && request.file().sha256().size() == 32 &&
           request.file().file_size() >= 0 && request.file().file_size() <= max_file_size && request.offset() >= 0 &&
           request.data().size() <= max_chunk_size;
}
bool valid(const ::file::DownloadRequest& request)
{
    return request.has_file() && request.user_id() > 0 && safe_id(request.file().file_id()) && request.offset() >= 0;
}
std::string object_key(int64_t uid, const std::string& id) { return std::to_string(uid) + "/" + id; }
unsigned owner_shard(const std::string& key, unsigned shards)
{
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char value : key) {
        hash = (hash ^ value) * 1099511628211ULL;
    }
    return static_cast<unsigned>(hash % shards);
}
} // namespace imm::file_service
