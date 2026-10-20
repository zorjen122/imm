#pragma once
#include "DbGlobal.h"
#include "Const.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace imm::db
{
struct MessageAcceptResult {
    int error{ErrorCodes::MysqlFailed};
    std::string diagnostic;
    bool duplicate{false};
    int64_t messageId{0};
    int64_t conversationId{0};
    int64_t conversationSeq{0};
};

struct GroupMessageAcceptResult : MessageAcceptResult {
    int64_t groupId{0};
    std::vector<int64_t> recipientUids;
};

struct GroupOperationResult {
    int error{ErrorCodes::MysqlFailed};
    std::string diagnostic;
    int64_t groupId{0};
    int64_t conversationId{0};
    int64_t masterUid{0};
    int64_t targetUid{0};
    int64_t actorUid{0};
    std::string groupName;
    std::string targetJoinTime;
    std::vector<int64_t> recipientUids;
    std::vector<GroupMember> members;
    std::vector<GroupApply> applies;
};

struct ConversationMessageRecord {
    int64_t messageId{0};
    int64_t senderId{0};
    int64_t receiverId{0};
    int64_t conversationId{0};
    int64_t conversationSeq{0};
    std::string clientMessageId;
    int type{0};
    std::string content;
    int status{0};
    std::string sendDateTime;
    std::string readDateTime;
    struct FileAttachment {
        std::string fileId;
        std::string fileName;
        std::string fileType;
        int64_t fileSize{0};
        std::string sha256;
        bool expired{false};
    };
    std::optional<FileAttachment> file;
};

struct FileObjectRecord {
    std::string fileId;
    long ownerUid{0};
    std::string fileName;
    std::string fileType;
    int64_t fileSize{0};
    std::string sha256;
    bool expired{false};
};

struct ConversationSyncResult {
    int error{ErrorCodes::MysqlFailed};
    int conversationType{0};
    int64_t latestSeq{0};
    int64_t nextSeq{0};
    bool hasMore{false};
    std::vector<ConversationMessageRecord> messages;
};

struct ConversationAckResult {
    int updated{-1};
    int64_t senderUid{0};
    bool directConversation{false};
    bool readStateChanged{false};
};

} // namespace imm::db
