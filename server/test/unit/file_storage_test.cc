#include "storage.hh"
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <openssl/sha.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
std::string digest(std::string_view bytes)
{
    std::string result(SHA256_DIGEST_LENGTH, '\0');
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
           reinterpret_cast<unsigned char*>(result.data()));
    return result;
}
::file::UploadRequest upload_request(const std::string& id, std::string_view content, int64_t offset, std::string bytes,
                                     bool complete)
{
    ::file::UploadRequest request;
    request.set_user_id(123);
    auto* info = request.mutable_file();
    info->set_file_id(id);
    info->set_file_name("test.bin");
    info->set_file_type("application/octet-stream");
    info->set_file_size(content.size());
    info->set_sha256(digest(content));
    request.set_offset(offset);
    request.set_data(std::move(bytes));
    request.set_complete(complete);
    return request;
}
seastar::future<> cases(imm::file_service::storage& store)
{
    const std::string id = "12345678-1234-1234-1234-123456789012";
    // 分块边界故意跨越 DMA 页，验证物理补齐不会改变协议恢复偏移或覆盖旧尾页。
    std::string content(8193, 'x');
    for (std::size_t index = 0; index < content.size(); ++index) {
        content[index] = static_cast<char>(index % 251);
    }
    auto first = co_await store.upload(upload_request(id, content, 0, content.substr(0, 3), false));
    require(first.status() == ::file::PARTIAL && first.next_offset() == 3, "unaligned first chunk");
    const auto retry = co_await store.upload(upload_request(id, content, 0, content.substr(0, 3), false));
    require(retry.status() == ::file::PARTIAL && retry.next_offset() == 3, "matching retry");
    const auto mismatch = co_await store.upload(upload_request(id, content, 0, "bad", false));
    require(mismatch.status() == ::file::FAILED, "mismatching retry");
    const auto second = co_await store.upload(upload_request(id, content, 3, content.substr(3, 4097), false));
    require(second.status() == ::file::PARTIAL && second.next_offset() == 4100, "cross-page append");
    ::file::DownloadRequest download;
    download.set_user_id(123);
    download.mutable_file()->set_file_id(id);
    download.set_limit(1024 * 1024);
    const auto pending = co_await store.download(download);
    require(pending.status() == ::file::FAILED, "partial file must not be downloaded");
    const auto completed = co_await store.upload(upload_request(id, content, 4100, content.substr(4100), true));
    require(completed.status() == ::file::SUCCESS && completed.next_offset() == 8193, "complete aligned hash");
    const auto received = co_await store.download(download);
    require(received.status() == ::file::SUCCESS && received.complete() && received.data() == content,
            "download bytes");
    const auto again = co_await store.upload(upload_request(id, content, 4100, content.substr(4100), true));
    require(again.status() == ::file::SUCCESS && again.next_offset() == 8193, "complete retry");
    const std::string empty_id = "12345678-1234-1234-1234-123456789013";
    const auto empty = co_await store.upload(upload_request(empty_id, "", 0, "", true));
    require(empty.status() == ::file::SUCCESS && empty.next_offset() == 0, "zero-byte upload");
    download.mutable_file()->set_file_id(empty_id);
    const auto empty_read = co_await store.download(download);
    require(empty_read.status() == ::file::SUCCESS && empty_read.complete() && empty_read.data().empty(),
            "zero-byte download");
    const std::string wrong_id = "12345678-1234-1234-1234-123456789014";
    const auto wrong = co_await store.upload(upload_request(wrong_id, "expected", 0, "badbytes", true));
    require(wrong.status() == ::file::FAILED, "wrong complete hash");
    download.mutable_file()->set_file_id(wrong_id);
    require((co_await store.download(download)).status() == ::file::FAILED, "checksum failure must not publish");
}
seastar::future<> run(std::filesystem::path root)
{
    imm::file_service::storage store(root);
    co_await store.start();
    std::exception_ptr failure;
    try {
        co_await cases(store);
    } catch (...) {
        failure = std::current_exception();
    }
    // 测试仅删除自身文件；所有清理同样通过 Seastar 文件 API 执行。
    for (const auto& suffix : {"123456789012", "123456789013", "123456789014.part"}) {
        co_await seastar::remove_file((root / "123" / (std::string("12345678-1234-1234-1234-") + suffix)).string())
            .handle_exception([](std::exception_ptr) {});
    }
    co_await seastar::remove_file((root / "123").string());
    co_await seastar::remove_file(root.string());
    if (failure) {
        std::rethrow_exception(failure);
    }
}
} // namespace
int main(int argc, char** argv)
{
    const auto root = std::filesystem::temp_directory_path() / ("imm-file-test-" + std::to_string(getpid()));
    seastar::app_template app;
    return app.run(argc, argv, [root] { return run(root); });
}
