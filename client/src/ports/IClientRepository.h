#pragma once

#include "domain/ClientSnapshot.h"

#include <QStringList>

#include <optional>

namespace wimi::client
{

class IClientRepository
{
  public:
    virtual ~IClientRepository() = default;

    virtual QStringList ScenarioNames() const = 0;
    virtual ClientSnapshot LoadScenario(const QString& scenarioName) const = 0;

    virtual QString RepositoryKind() const { return QStringLiteral("fake"); }
    virtual std::optional<std::int64_t> BoundUserId() const { return std::nullopt; }
    virtual bool BindUser(std::int64_t, const QString&) { return true; }
    virtual bool EnqueueOutgoing(const QString&, const MessageRecord&) { return true; }
    virtual bool UpdateDeliveryState(std::int64_t, MessageDeliveryState) { return true; }
    virtual bool AcceptOutgoing(std::int64_t clientMessageId, std::int64_t messageId, const QString& conversationId,
                                std::int64_t conversationSeq)
    {
        return UpdateDeliveryState(clientMessageId, MessageDeliveryState::Accepted);
    }
    virtual bool StoreDraft(const QString&, const QString&) { return true; }
    virtual bool SaveConversation(const ConversationRecord&) { return true; }
    // Removing a conversation from the active list deliberately leaves its
    // message rows intact so departed group history remains local/read-only.
    virtual bool RemoveConversation(const QString&) { return true; }
    virtual bool SetRemoteConversationId(const QString&, std::int64_t) { return true; }
    virtual bool ReplaceContacts(const QVector<ContactRecord>&) { return true; }
    virtual bool ReplaceRequests(const QVector<RequestRecord>&) { return true; }
    virtual bool ApplyIncomingBatch(const QString&, const QVector<MessageRecord>&, std::int64_t) { return true; }
    virtual std::int64_t SyncCursor(const QString&) const { return 0; }
    virtual QVector<FileUploadRecord> LoadFileUploads() const { return {}; }
    virtual bool SaveFileUpload(const FileUploadRecord&) { return true; }
    virtual bool RemoveFileUpload(const QString&) { return true; }
    virtual QVector<FileDownloadRecord> LoadFileDownloads() const { return {}; }
    virtual bool SaveFileDownload(const FileDownloadRecord&) { return true; }
    virtual bool RemoveFileDownload(const QString&) { return true; }
};

} // namespace wimi::client
