#include "storage.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/core/file.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <string_view>
#include <memory>
#include <optional>
#include <openssl/evp.h>
#include <stdexcept>
#include <system_error>

namespace imm::file_service
{
namespace
{
seastar::future<std::optional<uint64_t>> file_size(std::string path)
{
    try {
        const auto stat = co_await seastar::file_stat(path);
        co_return stat.size;
    } catch (const std::system_error& error) {
        if (error.code().value() == ENOENT) {
            co_return std::nullopt;
        }
        throw;
    }
}
seastar::future<std::string> read_range(seastar::file& file, uint64_t offset, std::size_t count)
{
    auto bytes = co_await file.dma_read_bulk<char>(offset, count);
    const auto size = std::min(count, bytes.size());
    co_return std::string(bytes.get(), size);
}
seastar::future<std::string> sha256(seastar::file& file, uint64_t size)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("cannot initialize sha256");
    }
    for (uint64_t offset = 0; offset < size;) {
        auto bytes = co_await read_range(file, offset, std::min<uint64_t>(65536, size - offset));
        if (bytes.empty() || EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1) {
            throw std::runtime_error("cannot hash file");
        }
        offset += bytes.size();
        // SHA 是 CPU 工作，每次只处理 64 KiB；已有就绪任务时让出 reactor。
        co_await seastar::coroutine::maybe_yield();
    }
    std::string digest(32, '\0');
    unsigned length{};
    if (EVP_DigestFinal_ex(context.get(), reinterpret_cast<unsigned char*>(digest.data()), &length) != 1 ||
        length != 32) {
        throw std::runtime_error("cannot finish sha256");
    }
    co_return digest;
}
// 无论业务、读写还是校验异常，均等待 close；不把未关闭的 DMA 文件留给析构处理。
template <typename Function> auto with_file(std::string path, seastar::open_flags flags, Function function)
{
    return seastar::open_file_dma(path, flags).then([function = std::move(function)](seastar::file file) mutable {
        return seastar::do_with(std::move(file), [function = std::move(function)](seastar::file& owned) mutable {
            return function(owned).finally([&owned] { return owned.close(); });
        });
    });
}
seastar::future<> append(seastar::file& file, uint64_t offset, std::string_view data)
{
    if (data.empty()) {
        co_return;
    }
    const auto alignment = file.disk_write_dma_alignment();
    const auto begin = offset / alignment * alignment;
    const auto end = (offset + data.size() + alignment - 1) / alignment * alignment;
    auto buffer = seastar::temporary_buffer<char>::aligned(file.memory_dma_alignment(), end - begin);
    std::memset(buffer.get_write(), 0, buffer.size());
    // 现有最后一页可能未对齐；先保留该页前缀，再追加新字节，不能以填零覆盖旧数据。
    if (begin < offset) {
        auto prefix = co_await read_range(file, begin, offset - begin);
        if (prefix.size() != offset - begin) {
            throw std::runtime_error("cannot read upload tail");
        }
        std::memcpy(buffer.get_write(), prefix.data(), prefix.size());
    }
    std::memcpy(buffer.get_write() + offset - begin, data.data(), data.size());
    const auto written = co_await file.dma_write(begin, buffer.get(), buffer.size());
    if (written != buffer.size()) {
        throw std::runtime_error("short file write");
    }
    // DMA 尾页补齐只属于物理写入要求；协议的恢复偏移必须仍然等于实际文件字节数。
    co_await file.truncate(offset + data.size());
}
seastar::future<std::string> write_and_hash(seastar::file& file, const ::file::UploadRequest& request, uint64_t current,
                                            uint64_t next)
{
    co_await append(file, current, request.data());
    if (request.complete() && next == static_cast<uint64_t>(request.file().file_size())) {
        co_return co_await sha256(file, next);
    }
    co_return std::string{};
}
} // namespace
seastar::future<> storage::start() { return seastar::recursive_touch_directory(_root.string()); }
seastar::future<::file::UploadResponse> storage::upload(::file::UploadRequest request)
{
    ::file::UploadResponse response;
    response.set_user_id(request.user_id());
    response.set_status(::file::FAILED);
    const auto object = (_root / object_key(request.user_id(), request.file().file_id())).string();
    const auto partial = object + ".part";
    co_await seastar::recursive_touch_directory((_root / std::to_string(request.user_id())).string());
    if (const auto completed = co_await file_size(object)) {
        const auto digest = co_await with_file(object, seastar::open_flags::ro,
                                               [size = *completed](seastar::file& file) { return sha256(file, size); });
        if (*completed != static_cast<uint64_t>(request.file().file_size()) || digest != request.file().sha256()) {
            response.set_message("completed file metadata mismatch");
        } else {
            response.set_status(::file::SUCCESS);
            response.set_next_offset(*completed);
        }
        co_return response;
    }
    const auto current = (co_await file_size(partial)).value_or(0);
    if (static_cast<uint64_t>(request.offset()) != current) {
        if (static_cast<uint64_t>(request.offset()) < current && !request.data().empty()) {
            const auto count = std::min<uint64_t>(request.data().size(), current - request.offset());
            const auto existing = co_await with_file(
                partial, seastar::open_flags::ro,
                [offset = request.offset(), count](seastar::file& file) { return read_range(file, offset, count); });
            if (existing != std::string_view(request.data()).substr(0, count)) {
                response.set_message("retried chunk does not match stored bytes");
                co_return response;
            }
        }
        response.set_status(::file::PARTIAL);
        response.set_next_offset(current);
        response.set_message("upload offset differs from stored progress");
        co_return response;
    }
    if (current > static_cast<uint64_t>(request.file().file_size()) ||
        request.data().size() > static_cast<uint64_t>(request.file().file_size()) - current) {
        response.set_message("chunk exceeds declared size");
        co_return response;
    }
    const auto next = current + request.data().size();
    const auto digest = co_await with_file(
        partial, seastar::open_flags::rw | seastar::open_flags::create,
        [&request, current, next](seastar::file& file) { return write_and_hash(file, request, current, next); });
    if (request.complete() && next == static_cast<uint64_t>(request.file().file_size())) {
        if (digest != request.file().sha256()) {
            response.set_message("file sha256 mismatch");
            co_return response;
        }
        co_await seastar::rename_file(partial, object);
        response.set_status(::file::SUCCESS);
    } else {
        response.set_status(::file::PARTIAL);
    }
    response.set_next_offset(next);
    co_return response;
}
seastar::future<::file::DownloadResponse> storage::download(::file::DownloadRequest request)
{
    ::file::DownloadResponse response;
    response.set_status(::file::FAILED);
    const auto object = (_root / object_key(request.user_id(), request.file().file_id())).string();
    const auto size = co_await file_size(object);
    if (!size) {
        response.set_message("file unavailable");
        co_return response;
    }
    if (static_cast<uint64_t>(request.offset()) > *size) {
        response.set_message("offset exceeds file size");
        co_return response;
    }
    const auto count = std::min<uint64_t>(std::max<int64_t>(request.limit(), 1),
                                          std::min<uint64_t>(max_chunk_size, *size - request.offset()));
    std::string data;
    if (count != 0) {
        data = co_await with_file(
            object, seastar::open_flags::ro,
            [offset = request.offset(), count](seastar::file& file) { return read_range(file, offset, count); });
        if (data.size() != count) {
            throw std::runtime_error("short file read");
        }
    }
    response.set_next_offset(request.offset() + data.size());
    response.set_complete(response.next_offset() == static_cast<int64_t>(*size));
    response.set_data(std::move(data));
    response.set_status(::file::SUCCESS);
    co_return response;
}
} // namespace imm::file_service
