#pragma once

#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>

namespace wimi::client
{

enum class MessageDeliveryState {
    PendingLocal,
    WaitingAccept,
    Accepted,
    Delivered,
    Read,
    Unknown,
    RetryableFailed,
    PermanentFailed,
};

struct ConversationRecord {
    QString conversationId;
    QString title;
    QString preview;
    QString timestamp;
    QString avatarColor;
    std::optional<std::int64_t> remoteConversationId;
    int unreadCount{};
    bool pinned{};
    bool muted{};
    bool online{};
};

struct MessageRecord {
    std::int64_t clientMessageId{};
    std::optional<std::int64_t> messageId;
    std::optional<std::int64_t> conversationSeq;
    QString senderId;
    QString body;
    int type{1};
    QString fileId;
    QString fileName;
    QString fileType;
    std::int64_t fileSize{};
    bool fileExpired{};
    QString timestamp;
    bool outgoing{};
    MessageDeliveryState deliveryState{MessageDeliveryState::Accepted};
};

struct FileUploadRecord {
    QString fileId;
    QString sourcePath;
    QString fileName;
    QString fileType;
    std::int64_t fileSize{};
    std::int64_t nextOffset{};
    QString conversationId;
    bool uploadComplete{};
};

struct FileDownloadRecord {
    QString fileId;
    QString fileType;
    QByteArray sha256;
    std::int64_t fileSize{};
    std::int64_t nextOffset{};
    QString temporaryPath;
};

struct ContactRecord {
    QString userId;
    QString displayName;
    QString statusText;
    QString avatarColor;
    bool online{};
    bool favorite{};
};

struct RequestRecord {
    QString requestId;
    QString displayName;
    QString message;
    QString avatarColor;
    QString kind;
    QString status;
};

struct ClientSnapshot {
    QString scenarioName;
    QString connectionStatus;
    QVector<ConversationRecord> conversations;
    QHash<QString, QVector<MessageRecord>> messagesByConversation;
    QVector<ContactRecord> contacts;
    QVector<RequestRecord> requests;
    QHash<QString, QString> draftsByConversation;
};

QString DeliveryStateName(MessageDeliveryState state);
bool CanTransition(MessageDeliveryState from, MessageDeliveryState to);

} // namespace wimi::client
