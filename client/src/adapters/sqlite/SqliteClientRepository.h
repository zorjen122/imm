#pragma once

#include "ports/IClientRepository.h"

#include <QSqlDatabase>

namespace wimi::client
{

class SqliteClientRepository final : public IClientRepository
{
  public:
    static constexpr int kCurrentSchemaVersion = 12;

    explicit SqliteClientRepository(QString databasePath, bool seedDemoData = true);
    ~SqliteClientRepository() override;

    SqliteClientRepository(const SqliteClientRepository&) = delete;
    SqliteClientRepository& operator=(const SqliteClientRepository&) = delete;

    QStringList ScenarioNames() const override;
    ClientSnapshot LoadScenario(const QString& scenarioName) const override;
    QString RepositoryKind() const override;
    std::optional<std::int64_t> BoundUserId() const override;
    bool BindUser(std::int64_t uid, const QString& username) override;
    bool EnqueueOutgoing(const QString& conversationId, const MessageRecord& message) override;
    bool UpdateDeliveryState(std::int64_t clientMessageId, MessageDeliveryState state) override;
    bool AcceptOutgoing(std::int64_t clientMessageId, std::int64_t messageId, const QString& conversationId,
                        std::int64_t conversationSeq) override;
    bool StoreDraft(const QString& conversationId, const QString& draft) override;
    bool SaveConversation(const ConversationRecord& conversation) override;
    bool RemoveConversation(const QString& conversationId) override;
    bool SetRemoteConversationId(const QString& conversationId, std::int64_t remoteConversationId) override;
    bool ReplaceContacts(const QVector<ContactRecord>& contacts) override;
    bool ReplaceRequests(const QVector<RequestRecord>& requests) override;

    bool IsReady() const;
    QString LastError() const;
    QString DatabasePath() const;

    bool ApplyIncomingBatch(const QString& conversationId, const QVector<MessageRecord>& messages,
                            std::int64_t nextCursor) override;
    std::int64_t SyncCursor(const QString& conversationId) const override;
    int OutboxCount() const;
    QVector<FileUploadRecord> LoadFileUploads() const override;
    bool SaveFileUpload(const FileUploadRecord& upload) override;
    bool RemoveFileUpload(const QString& fileId) override;
    QVector<FileDownloadRecord> LoadFileDownloads() const override;
    bool SaveFileDownload(const FileDownloadRecord& download) override;
    bool RemoveFileDownload(const QString& fileId) override;

  private:
    bool Open();
    bool Migrate();
    bool SeedIfEmpty();
    bool InsertSnapshot(const ClientSnapshot& snapshot);
    bool InsertMessage(const QString& conversationId, const MessageRecord& message, bool ignoreDuplicate);
    void SetError(const QString& error) const;

    QString database_path_;
    QString connection_name_;
    QSqlDatabase database_;
    mutable QString last_error_;
    bool ready_{};
    bool seed_demo_data_{true};
};

} // namespace wimi::client
