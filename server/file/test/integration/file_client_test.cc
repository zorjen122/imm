#include "Service.h"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <openssl/sha.h>

namespace fs = std::filesystem;

namespace wimi::rpc::test
{
namespace
{
constexpr char kFileId[] = "12345678-1234-1234-1234-123456789012";

std::string Sha256(std::string_view value)
{
    std::string digest(SHA256_DIGEST_LENGTH, '\0');
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(),
           reinterpret_cast<unsigned char*>(digest.data()));
    return digest;
}

file::FileInfo Metadata(int64_t size)
{
    file::FileInfo file;
    file.set_file_id(kFileId);
    file.set_file_name("test.bin");
    file.set_file_type("application/octet-stream");
    file.set_file_size(size);
    file.set_sha256(Sha256("helloworld"));
    return file;
}
} // namespace

class FileClientTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root = fs::temp_directory_path() / ("wimi-file-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        service = std::make_unique<FileServiceImpl>(root);
    }

    void TearDown() override
    {
        fs::remove_all(root);
    }

    file::UploadResponse Upload(int64_t offset, std::string data, bool complete)
    {
        grpc::ServerContext context;
        file::UploadRequest request;
        *request.mutable_file() = Metadata(10);
        request.set_user_id(123);
        request.set_offset(offset);
        request.set_data(std::move(data));
        request.set_complete(complete);
        file::UploadResponse response;
        EXPECT_TRUE(service->Upload(&context, &request, &response).ok());
        return response;
    }

    fs::path root;
    std::unique_ptr<FileServiceImpl> service;
};

TEST_F(FileClientTest, UploadResumesFromAcknowledgedOffsetAndDownloads)
{
    const auto head = Upload(0, {}, false);
    EXPECT_EQ(head.status(), file::PARTIAL);
    EXPECT_EQ(head.next_offset(), 0);

    const auto first = Upload(0, "hello", false);
    EXPECT_EQ(first.status(), file::PARTIAL);
    EXPECT_EQ(first.next_offset(), 5);

    const auto resumed = Upload(0, "hello", false);
    EXPECT_EQ(resumed.status(), file::PARTIAL);
    EXPECT_EQ(resumed.next_offset(), 5);

    const auto mismatchedRetry = Upload(0, "HELLO", false);
    EXPECT_EQ(mismatchedRetry.status(), file::FAILED);

    const auto overlappingRetry = Upload(3, "loWORLD", false);
    EXPECT_EQ(overlappingRetry.status(), file::PARTIAL);
    EXPECT_EQ(overlappingRetry.next_offset(), 5);

    const auto mismatchedOverlap = Upload(3, "xxWORLD", false);
    EXPECT_EQ(mismatchedOverlap.status(), file::FAILED);

    const auto completed = Upload(5, "world", true);
    EXPECT_EQ(completed.status(), file::SUCCESS);
    EXPECT_EQ(completed.next_offset(), 10);
    EXPECT_TRUE(fs::exists(root / "123" / kFileId));

    grpc::ServerContext context;
    file::DownloadRequest request;
    *request.mutable_file() = Metadata(10);
    request.set_user_id(123);
    request.set_offset(0);
    request.set_limit(1024);
    file::DownloadResponse response;
    EXPECT_TRUE(service->Download(&context, &request, &response).ok());
    EXPECT_EQ(response.status(), file::SUCCESS);
    EXPECT_EQ(response.data(), "helloworld");
    EXPECT_TRUE(response.complete());
}

TEST_F(FileClientTest, RejectsInvalidMetadataAndChecksum)
{
    grpc::ServerContext context;
    file::UploadRequest invalid;
    *invalid.mutable_file() = Metadata(10);
    invalid.mutable_file()->clear_sha256();
    invalid.set_user_id(123);
    invalid.set_offset(0);
    file::UploadResponse invalidResponse;
    EXPECT_TRUE(service->Upload(&context, &invalid, &invalidResponse).ok());
    EXPECT_EQ(invalidResponse.status(), file::FAILED);

    file::UploadRequest wrongChecksum;
    *wrongChecksum.mutable_file() = Metadata(10);
    wrongChecksum.mutable_file()->set_sha256(Sha256("different!"));
    wrongChecksum.set_user_id(123);
    wrongChecksum.set_offset(0);
    wrongChecksum.set_data("helloworld");
    wrongChecksum.set_complete(true);
    file::UploadResponse wrongChecksumResponse;
    EXPECT_TRUE(service->Upload(&context, &wrongChecksum, &wrongChecksumResponse).ok());
    EXPECT_EQ(wrongChecksumResponse.status(), file::FAILED);
    EXPECT_FALSE(fs::exists(root / "123" / kFileId));
}

} // namespace wimi::rpc::test
