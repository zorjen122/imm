#include "adapters/fake/FakeScenarioRepository.h"
#include "adapters/sqlite/SqliteClientRepository.h"
#include "adapters/connection_gateway/TcpFrameCodec.h"
#include "app/AppController.h"
#include "models/ConversationListModel.h"
#include "models/MessageListModel.h"

#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QIODevice>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

namespace wimi::client
{
namespace
{

QString TestConnectionName()
{
    return QStringLiteral("wimi-client-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

bool ExecuteSqliteStatements(const QString& databasePath, const QStringList& statements)
{
    const QString connectionName = TestConnectionName();
    bool success = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            success = true;
            for (const auto& statement : statements) {
                QSqlQuery query(database);
                if (!query.exec(statement)) {
                    success = false;
                    break;
                }
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return success;
}

int SqliteSchemaVersion(const QString& databasePath)
{
    const QString connectionName = TestConnectionName();
    int version = -1;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next()) {
                version = query.value(0).toInt();
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return version;
}

bool SqliteIndexExists(const QString& databasePath, const QString& indexName)
{
    const QString connectionName = TestConnectionName();
    bool exists = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            query.prepare(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type = "
                                         "'index' AND name = ?"));
            query.addBindValue(indexName);
            exists = query.exec() && query.next() && query.value(0).toInt() == 1;
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return exists;
}

class RecordingPlatformServices final : public IPlatformServices
{
  public:
    QString PlatformName() const override { return QStringLiteral("test-linux"); }

    bool DesktopNotificationsAvailable() const override { return true; }

    bool ShowDesktopNotification(const QString& title, const QString& body) override
    {
        ++notificationCount;
        lastTitle = title;
        lastBody = body;
        return showResult;
    }

    int notificationCount{};
    QString lastTitle;
    QString lastBody;
    bool showResult{true};
};

} // namespace

class ClientModelsTest final : public QObject
{
    Q_OBJECT

  private slots:
    void fakeRepositoryExposesEveryPlannedScenario();
    void emptyAndOfflineScenariosAreDeterministic();
    void largeHistoryScenarioExercisesModelScale();
    void conversationRolesExposeSnapshotData();
    void conversationUnreadCountUpdatesByConversation();
    void fileMessageRolesExposeAttachmentIdentity();
    void deliveryStateRejectsRegression();
    void draftsAreScopedToConversation();
    void sendLifecycleSurvivesConversationSwitch();
    void retryableMessageKeepsItsClientIdentity();
    void contactAndRequestModelsUpdateInPlace();
    void fakeServiceActionsProvideCompleteUiFeedback();
    void fakeModeExplainsWhyFileTransferIsUnavailable();
    void sqlitePersistsOutboxDraftAndSyncCursor();
    void sqliteMigratesVersionOneToCurrent();
    void sqliteBindsOneServerIdentity();
    void sqlitePersistsFileUploadProgress();
    void sqlitePersistsFileDownloadProgress();
    void sqliteRejectsNewerSchema();
    void sqliteAppliesLargeIncomingBatchIdempotently();
    void sqliteLoadsMessagesByConversationSequence();
    void sqliteArchivesConversationWithoutDeletingHistory();
    void sqlitePersistsNetworkProjection();
    void platformNotificationPortUpdatesControllerState();
    void tcpFrameCodecHandlesFragmentationAndCoalescing();
    void tcpFrameCodecRejectsOversizedPayload();
};

void ClientModelsTest::fakeRepositoryExposesEveryPlannedScenario()
{
    const FakeScenarioRepository repository;
    const auto scenarios = repository.ScenarioNames();

    QCOMPARE(scenarios.size(), 13);
    QVERIFY(scenarios.contains(QStringLiteral("normal")));
    QVERIFY(scenarios.contains(QStringLiteral("send-lifecycle")));
    QVERIFY(scenarios.contains(QStringLiteral("sync-gap")));
    QVERIFY(scenarios.contains(QStringLiteral("long-content")));
    QVERIFY(scenarios.contains(QStringLiteral("large-history")));
}

void ClientModelsTest::largeHistoryScenarioExercisesModelScale()
{
    const FakeScenarioRepository repository;
    QElapsedTimer timer;
    timer.start();
    const auto snapshot = repository.LoadScenario(QStringLiteral("large-history"));
    const qint64 scenarioLoadMilliseconds = timer.elapsed();

    const auto records = snapshot.messagesByConversation.value(QStringLiteral("alice"));
    QCOMPARE(records.size(), 2005);

    MessageListModel model;
    timer.restart();
    model.SetRecords(records);
    const qint64 modelLoadMilliseconds = timer.elapsed();
    QCOMPARE(model.rowCount(), 2005);
    QVERIFY2(scenarioLoadMilliseconds < 5000,
             qPrintable(QStringLiteral("large scenario load took %1 ms").arg(scenarioLoadMilliseconds)));
    QVERIFY2(modelLoadMilliseconds < 5000,
             qPrintable(QStringLiteral("large model load took %1 ms").arg(modelLoadMilliseconds)));
}

void ClientModelsTest::emptyAndOfflineScenariosAreDeterministic()
{
    const FakeScenarioRepository repository;

    const auto empty = repository.LoadScenario(QStringLiteral("empty-account"));
    QCOMPARE(empty.connectionStatus, QStringLiteral("online"));
    QVERIFY(empty.conversations.isEmpty());

    const auto offline = repository.LoadScenario(QStringLiteral("offline-cached"));
    QCOMPARE(offline.connectionStatus, QStringLiteral("offline-cached"));
    QVERIFY(!offline.conversations.isEmpty());
}

void ClientModelsTest::conversationRolesExposeSnapshotData()
{
    ConversationListModel model;
    model.SetRecords({ConversationRecord{
        .conversationId = QStringLiteral("conversation-1"),
        .title = QStringLiteral("测试会话"),
        .preview = QStringLiteral("预览"),
        .timestamp = QStringLiteral("12:30"),
        .avatarColor = QStringLiteral("#315FD6"),
        .unreadCount = 4,
        .pinned = true,
        .muted = false,
        .online = true,
    }});

    QCOMPARE(model.rowCount(), 1);
    const auto index = model.index(0);
    QCOMPARE(model.data(index, ConversationListModel::TitleRole).toString(), QStringLiteral("测试会话"));
    QCOMPARE(model.data(index, ConversationListModel::SourceIndexRole).toInt(), 0);
    QCOMPARE(model.data(index, ConversationListModel::UnreadCountRole).toInt(), 4);
    QCOMPARE(model.data(index, ConversationListModel::PinnedRole).toBool(), true);
}

void ClientModelsTest::conversationUnreadCountUpdatesByConversation()
{
    ConversationListModel model;
    model.SetRecords({ConversationRecord{
        .conversationId = QStringLiteral("direct:42"),
        .title = QStringLiteral("用户 42"),
        .unreadCount = 1,
    }});

    QVERIFY(model.SetUnreadCount(QStringLiteral("direct:42"), 2));
    QCOMPARE(model.data(model.index(0), ConversationListModel::UnreadCountRole).toInt(), 2);
    QVERIFY(!model.SetUnreadCount(QStringLiteral("direct:42"), 2));
    QVERIFY(!model.SetUnreadCount(QStringLiteral("direct:404"), 1));
}

void ClientModelsTest::fileMessageRolesExposeAttachmentIdentity()
{
    MessageListModel model;
    model.SetRecords({MessageRecord{
        .clientMessageId = 88,
        .messageId = 88,
        .senderId = QStringLiteral("alice"),
        .body = QStringLiteral("5e3f43da-0d67-4e4a-99a0-6ccff21892a5"),
        .type = 5,
        .fileId = QStringLiteral("5e3f43da-0d67-4e4a-99a0-6ccff21892a5"),
        .fileName = QStringLiteral("notes.md"),
        .fileType = QStringLiteral("text/markdown"),
        .fileSize = 17,
        .timestamp = QStringLiteral("2026-08-08 12:00:00"),
        .outgoing = false,
    }});

    const auto index = model.index(0);
    QCOMPARE(model.data(index, MessageListModel::TypeRole).toInt(), 5);
    QCOMPARE(model.data(index, MessageListModel::FileIdRole).toString(),
             QStringLiteral("5e3f43da-0d67-4e4a-99a0-6ccff21892a5"));
    QCOMPARE(model.data(index, MessageListModel::FileNameRole).toString(), QStringLiteral("notes.md"));
    QCOMPARE(model.data(index, MessageListModel::FileTypeRole).toString(), QStringLiteral("text/markdown"));
    QCOMPARE(model.data(index, MessageListModel::FileSizeRole).toLongLong(), 17);
}

void ClientModelsTest::deliveryStateRejectsRegression()
{
    MessageListModel model;
    model.SetRecords({MessageRecord{
                          .clientMessageId = -2,
                          .senderId = QStringLiteral("alice"),
                          .body = QStringLiteral("yesterday"),
                          .timestamp = QStringLiteral("2026-07-17 23:59:00"),
                          .outgoing = false,
                          .deliveryState = MessageDeliveryState::Read,
                      },
                      MessageRecord{
                          .clientMessageId = -1,
                          .senderId = QStringLiteral("me"),
                          .body = QStringLiteral("hello"),
                          .timestamp = QStringLiteral("2026-07-18 12:30:00"),
                          .outgoing = true,
                          .deliveryState = MessageDeliveryState::PendingLocal,
                      }},
                     1);

    const auto pendingIndex = model.index(1);
    QCOMPARE(model.data(pendingIndex, MessageListModel::TimestampRole).toString(), QStringLiteral("12:30"));
    QCOMPARE(model.data(pendingIndex, MessageListModel::SourceIndexRole).toInt(), 1);
    QVERIFY(model.data(pendingIndex, MessageListModel::ShowDateSeparatorRole).toBool());
    QVERIFY(model.data(pendingIndex, MessageListModel::ShowUnreadSeparatorRole).toBool());

    QVERIFY(model.UpdateDeliveryState(-1, MessageDeliveryState::WaitingAccept));
    QVERIFY(model.UpdateDeliveryState(-1, MessageDeliveryState::Accepted));
    QVERIFY(model.UpdateDeliveryState(-1, MessageDeliveryState::Delivered));
    QVERIFY(!model.UpdateDeliveryState(-1, MessageDeliveryState::Accepted));
    QVERIFY(model.UpdateDeliveryState(-1, MessageDeliveryState::Read));
    QVERIFY(!model.UpdateDeliveryState(-1, MessageDeliveryState::RetryableFailed));
    QCOMPARE(model.data(pendingIndex, MessageListModel::DeliveryStateRole).toString(), QStringLiteral("read"));
}

void ClientModelsTest::draftsAreScopedToConversation()
{
    AppController controller;

    controller.setDraftText(QStringLiteral("林晓草稿"));
    controller.selectConversation(1);
    QVERIFY(controller.selectedConversationIsGroup());
    QCOMPARE(controller.draftText(), QString{});
    controller.setDraftText(QStringLiteral("设计组草稿"));

    controller.selectConversation(0);
    QVERIFY(!controller.selectedConversationIsGroup());
    QCOMPARE(controller.draftText(), QStringLiteral("林晓草稿"));
    controller.selectConversation(1);
    QCOMPARE(controller.draftText(), QStringLiteral("设计组草稿"));
}

void ClientModelsTest::sendLifecycleSurvivesConversationSwitch()
{
    AppController controller;
    const int initialCount = controller.messages()->rowCount();

    controller.sendMessage(QStringLiteral("状态机测试"));
    QCOMPARE(controller.messages()->rowCount(), initialCount + 1);
    QCOMPARE(controller.messages()
                 ->data(controller.messages()->index(initialCount), MessageListModel::DeliveryStateRole)
                 .toString(),
             QStringLiteral("pending"));

    controller.selectConversation(1);
    QTest::qWait(2500);
    controller.selectConversation(0);

    QCOMPARE(controller.messages()->rowCount(), initialCount + 1);
    QCOMPARE(controller.messages()
                 ->data(controller.messages()->index(initialCount), MessageListModel::DeliveryStateRole)
                 .toString(),
             QStringLiteral("read"));
}

void ClientModelsTest::retryableMessageKeepsItsClientIdentity()
{
    AppController controller;
    controller.setScenarioName(QStringLiteral("send-lifecycle"));

    const int originalCount = controller.messages()->rowCount();
    const auto lastIndex = controller.messages()->index(originalCount - 1);
    QCOMPARE(controller.messages()->data(lastIndex, MessageListModel::ClientMessageIdRole).toLongLong(), -90);
    QCOMPARE(controller.messages()->data(lastIndex, MessageListModel::DeliveryStateRole).toString(),
             QStringLiteral("retryable-failed"));

    controller.retryMessage(-90);

    QCOMPARE(controller.messages()->rowCount(), originalCount);
    QCOMPARE(controller.messages()->data(lastIndex, MessageListModel::ClientMessageIdRole).toLongLong(), -90);
    QCOMPARE(controller.messages()->data(lastIndex, MessageListModel::DeliveryStateRole).toString(),
             QStringLiteral("pending"));
}

void ClientModelsTest::contactAndRequestModelsUpdateInPlace()
{
    AppController controller;

    controller.setCurrentSection(QStringLiteral("requests"));
    QCOMPARE(controller.currentSection(), QStringLiteral("requests"));

    QVERIFY(controller.contacts()->rowCount() > 0);
    QVERIFY(controller.requests()->rowCount() > 0);

    const auto favoriteIndex = controller.contacts()->index(0);
    QCOMPARE(controller.contacts()->data(favoriteIndex, ContactListModel::SourceIndexRole).toInt(), 0);
    const bool oldFavorite = controller.contacts()->data(favoriteIndex, ContactListModel::FavoriteRole).toBool();
    controller.toggleContactFavorite(0);
    QCOMPARE(controller.contacts()->data(favoriteIndex, ContactListModel::FavoriteRole).toBool(), !oldFavorite);

    const auto requestIndex = controller.requests()->index(0);
    QCOMPARE(controller.requests()->pendingCount(), 1);
    QCOMPARE(controller.requests()->resolvedCount(), 2);
    QCOMPARE(controller.requests()->data(requestIndex, RequestListModel::StatusRole).toString(),
             QStringLiteral("pending"));
    QVERIFY(controller.requests()->SetStatus(0, QStringLiteral("accepting")));
    QCOMPARE(controller.requests()->pendingCount(), 1);
    QCOMPARE(controller.requests()->resolvedCount(), 2);
    QVERIFY(controller.requests()->SetStatus(0, QStringLiteral("pending")));
    controller.resolveRequest(0, true);
    QCOMPARE(controller.requests()->pendingCount(), 0);
    QCOMPARE(controller.requests()->resolvedCount(), 3);
    QCOMPARE(controller.requests()->data(requestIndex, RequestListModel::StatusRole).toString(),
             QStringLiteral("accepted"));
    controller.resolveRequest(0, false);
    QCOMPARE(controller.requests()->data(requestIndex, RequestListModel::StatusRole).toString(),
             QStringLiteral("accepted"));
}

void ClientModelsTest::fakeServiceActionsProvideCompleteUiFeedback()
{
    AppController controller;

    controller.sendFriendRequest(QStringLiteral("42"), QStringLiteral("你好"));
    QVERIFY(controller.serviceActionStatus().contains(QStringLiteral("42")));

    const int originalConversationCount = controller.conversations()->rowCount();
    controller.setCurrentSection(QStringLiteral("contacts"));
    controller.createGroup(QStringLiteral("第一阶段演示群"));
    QCOMPARE(controller.conversations()->rowCount(), originalConversationCount + 1);
    QCOMPARE(controller.currentSection(), QStringLiteral("chats"));
    QCOMPARE(controller.selectedConversationTitle(), QStringLiteral("第一阶段演示群"));

    controller.joinGroup(QStringLiteral("9001"), QString{});
    QVERIFY(controller.serviceActionStatus().contains(QStringLiteral("9001")));
}

void ClientModelsTest::fakeModeExplainsWhyFileTransferIsUnavailable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString filePath = directory.filePath(QStringLiteral("notes.md"));
    QFile file(filePath);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("# notes\n") > 0);
    file.close();

    AppController controller;
    controller.sendFile(QUrl::fromLocalFile(filePath));
    QVERIFY(controller.serviceActionStatus().contains(QStringLiteral("演示模式")));
}

void ClientModelsTest::sqlitePersistsOutboxDraftAndSyncCursor()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("account.sqlite"));

    const MessageRecord outgoing{
        .clientMessageId = -9000,
        .senderId = QStringLiteral("me"),
        .body = QStringLiteral("persist me"),
        .timestamp = QStringLiteral("13:00"),
        .outgoing = true,
        .deliveryState = MessageDeliveryState::PendingLocal,
    };

    {
        SqliteClientRepository repository(databasePath);
        QVERIFY2(repository.IsReady(), qPrintable(repository.LastError()));
        QVERIFY(repository.EnqueueOutgoing(QStringLiteral("alice"), outgoing));
        QCOMPARE(repository.OutboxCount(), 1);
        QVERIFY(repository.StoreDraft(QStringLiteral("alice"), QStringLiteral("durable draft")));
    }

    {
        SqliteClientRepository repository(databasePath);
        QVERIFY2(repository.IsReady(), qPrintable(repository.LastError()));
        const auto restored = repository.LoadScenario(QStringLiteral("local-sqlite"));
        QCOMPARE(restored.draftsByConversation.value(QStringLiteral("alice")), QStringLiteral("durable draft"));

        bool outgoingRestored = false;
        for (const auto& message : restored.messagesByConversation.value(QStringLiteral("alice"))) {
            if (message.clientMessageId == outgoing.clientMessageId) {
                outgoingRestored = true;
                QCOMPARE(message.deliveryState, MessageDeliveryState::PendingLocal);
            }
        }
        QVERIFY(outgoingRestored);
        QCOMPARE(repository.OutboxCount(), 1);

        QVERIFY(repository.ApplyIncomingBatch(QStringLiteral("alice"), {}, 42));
        QVERIFY(repository.UpdateDeliveryState(outgoing.clientMessageId, MessageDeliveryState::WaitingAccept));
        QVERIFY2(repository.AcceptOutgoing(outgoing.clientMessageId, 9000, QStringLiteral("alice"), 43),
                 qPrintable(repository.LastError()));
        QCOMPARE(repository.OutboxCount(), 0);
        // ACCEPTED only assigns the server identity. The persisted cursor advances
        // after that exact sequence has been committed as part of the contiguous
        // local conversation stream.
        QCOMPARE(repository.SyncCursor(QStringLiteral("alice")), 42);
        QVERIFY(repository.ApplyIncomingBatch(QStringLiteral("alice"), {}, 43));
        QCOMPARE(repository.SyncCursor(QStringLiteral("alice")), 43);

        const auto afterAccept = repository.LoadScenario(QStringLiteral("local-sqlite"));
        bool acceptedRestored = false;
        for (const auto& message : afterAccept.messagesByConversation.value(QStringLiteral("alice"))) {
            if (message.clientMessageId == outgoing.clientMessageId) {
                acceptedRestored = true;
                QCOMPARE(message.messageId, std::optional<std::int64_t>(9000));
                QCOMPARE(message.conversationSeq, std::optional<std::int64_t>(43));
                QCOMPARE(message.deliveryState, MessageDeliveryState::Accepted);
            }
        }
        QVERIFY(acceptedRestored);

        const MessageRecord incoming{
            .clientMessageId = 9001,
            .messageId = 9001,
            .conversationSeq = 44,
            .senderId = QStringLiteral("alice"),
            .body = QStringLiteral("synced once"),
            .timestamp = QStringLiteral("13:01"),
            .outgoing = false,
            .deliveryState = MessageDeliveryState::Delivered,
        };
        QVERIFY(repository.ApplyIncomingBatch(QStringLiteral("alice"), {incoming}, 44));
        QVERIFY(repository.ApplyIncomingBatch(QStringLiteral("alice"), {incoming}, 44));
        QCOMPARE(repository.SyncCursor(QStringLiteral("alice")), 44);

        const auto afterSync = repository.LoadScenario(QStringLiteral("local-sqlite"));
        int matchingMessages = 0;
        for (const auto& message : afterSync.messagesByConversation.value(QStringLiteral("alice"))) {
            if (message.messageId == incoming.messageId) {
                ++matchingMessages;
            }
        }
        QCOMPARE(matchingMessages, 1);
    }
}

void ClientModelsTest::sqliteMigratesVersionOneToCurrent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("migration.sqlite"));

    {
        SqliteClientRepository repository(databasePath);
        QVERIFY2(repository.IsReady(), qPrintable(repository.LastError()));
        QVERIFY(!repository.LoadScenario(QStringLiteral("local-sqlite")).conversations.isEmpty());
    }
    QCOMPARE(SqliteSchemaVersion(databasePath), SqliteClientRepository::kCurrentSchemaVersion);

    QVERIFY(ExecuteSqliteStatements(databasePath,
                                    {
                                        QStringLiteral("DROP INDEX IF EXISTS idx_messages_conversation_local_order"),
                                        QStringLiteral("DROP INDEX IF EXISTS idx_outbox_retry"),
                                        QStringLiteral("DROP INDEX IF EXISTS idx_conversations_sort"),
                                        QStringLiteral("PRAGMA user_version = 1"),
                                    }));
    QCOMPARE(SqliteSchemaVersion(databasePath), 1);
    QVERIFY(!SqliteIndexExists(databasePath, QStringLiteral("idx_messages_conversation_local_order")));

    {
        SqliteClientRepository repository(databasePath, false);
        QVERIFY2(repository.IsReady(), qPrintable(repository.LastError()));
        QVERIFY(!repository.LoadScenario(QStringLiteral("local-sqlite")).conversations.isEmpty());
    }

    QCOMPARE(SqliteSchemaVersion(databasePath), SqliteClientRepository::kCurrentSchemaVersion);
    QVERIFY(SqliteIndexExists(databasePath, QStringLiteral("idx_messages_conversation_local_order")));
    QVERIFY(SqliteIndexExists(databasePath, QStringLiteral("idx_outbox_retry")));
    QVERIFY(SqliteIndexExists(databasePath, QStringLiteral("idx_conversations_sort")));
}

void ClientModelsTest::sqliteBindsOneServerIdentity()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("identity.sqlite"));

    {
        SqliteClientRepository repository(databasePath, false);
        QVERIFY2(repository.IsReady(), qPrintable(repository.LastError()));
        QVERIFY(!repository.BoundUserId().has_value());
        QVERIFY(repository.BindUser(1002, QStringLiteral("alice")));
        QCOMPARE(repository.BoundUserId(), std::optional<std::int64_t>(1002));
        QVERIFY(repository.BindUser(1002, QStringLiteral("alice-renamed")));
        QVERIFY(!repository.BindUser(1001, QStringLiteral("zorjen")));
        QCOMPARE(repository.BoundUserId(), std::optional<std::int64_t>(1002));
    }

    SqliteClientRepository restoredRepository(databasePath, false);
    QVERIFY2(restoredRepository.IsReady(), qPrintable(restoredRepository.LastError()));
    QCOMPARE(restoredRepository.BoundUserId(), std::optional<std::int64_t>(1002));
}

void ClientModelsTest::sqlitePersistsFileUploadProgress()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("file-upload.sqlite"));
    {
        SqliteClientRepository repository(databasePath, false);
        QVERIFY2(repository.IsReady(), qPrintable(repository.LastError()));
        const FileUploadRecord upload{.fileId = QStringLiteral("5e3f43da-0d67-4e4a-99a0-6ccff21892a5"),
                                      .sourcePath = QStringLiteral("/tmp/report.md"),
                                      .fileName = QStringLiteral("report.md"),
                                      .fileType = QStringLiteral("text/markdown"),
                                      .fileSize = 1'500'000,
                                      .nextOffset = 1'048'576,
                                      .conversationId = QStringLiteral("direct:42"),
                                      .uploadComplete = true};
        QVERIFY(repository.SaveFileUpload(upload));
        const auto uploads = repository.LoadFileUploads();
        QCOMPARE(uploads.size(), 1);
        QCOMPARE(uploads.front().fileId, upload.fileId);
        QCOMPARE(uploads.front().nextOffset, upload.nextOffset);
        QVERIFY(uploads.front().uploadComplete);
        QVERIFY(repository.RemoveFileUpload(upload.fileId));
        QVERIFY(repository.LoadFileUploads().isEmpty());
    }
}

void ClientModelsTest::sqlitePersistsFileDownloadProgress()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SqliteClientRepository repository(directory.filePath(QStringLiteral("file-download.sqlite")), false);
    QVERIFY2(repository.IsReady(), qPrintable(repository.LastError()));
    const FileDownloadRecord download{.fileId = QStringLiteral("5e3f43da-0d67-4e4a-99a0-6ccff21892a5"),
                                      .fileType = QStringLiteral("image/png"),
                                      .sha256 = QByteArray(32, '\x1a'),
                                      .fileSize = 2'000'000,
                                      .nextOffset = 1'048'576,
                                      .temporaryPath = QStringLiteral("/tmp/file.part")};
    QVERIFY(repository.SaveFileDownload(download));
    const auto downloads = repository.LoadFileDownloads();
    QCOMPARE(downloads.size(), 1);
    QCOMPARE(downloads.front().nextOffset, download.nextOffset);
    QCOMPARE(downloads.front().sha256, download.sha256);
    QVERIFY(repository.RemoveFileDownload(download.fileId));
    QVERIFY(repository.LoadFileDownloads().isEmpty());
}

void ClientModelsTest::sqliteRejectsNewerSchema()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("newer.sqlite"));
    const int newerVersion = SqliteClientRepository::kCurrentSchemaVersion + 1;

    QVERIFY(ExecuteSqliteStatements(databasePath, {QStringLiteral("PRAGMA user_version = %1").arg(newerVersion)}));

    SqliteClientRepository repository(databasePath, false);
    QVERIFY(!repository.IsReady());
    QVERIFY2(repository.LastError().contains(QStringLiteral("newer")), qPrintable(repository.LastError()));
    QCOMPARE(SqliteSchemaVersion(databasePath), newerVersion);
}

void ClientModelsTest::sqliteAppliesLargeIncomingBatchIdempotently()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("large-sync.sqlite"));
    SqliteClientRepository repository(databasePath);
    QVERIFY2(repository.IsReady(), qPrintable(repository.LastError()));

    constexpr int kMessageCount = 2000;
    constexpr std::int64_t kFirstSequence = 1000;
    QVector<MessageRecord> incoming;
    incoming.reserve(kMessageCount);
    for (int index = 0; index < kMessageCount; ++index) {
        const std::int64_t identity = 200000 + index;
        incoming.push_back(MessageRecord{
            .clientMessageId = identity,
            .messageId = identity,
            .conversationSeq = kFirstSequence + index,
            .senderId = QStringLiteral("alice"),
            .body = QStringLiteral("sync batch %1").arg(index),
            .timestamp = QStringLiteral("14:00"),
            .outgoing = false,
            .deliveryState = MessageDeliveryState::Delivered,
        });
    }
    const std::int64_t finalCursor = kFirstSequence + kMessageCount - 1;

    QElapsedTimer timer;
    timer.start();
    QVERIFY2(repository.ApplyIncomingBatch(QStringLiteral("alice"), incoming, finalCursor),
             qPrintable(repository.LastError()));
    QVERIFY2(repository.ApplyIncomingBatch(QStringLiteral("alice"), incoming, finalCursor),
             qPrintable(repository.LastError()));
    const qint64 elapsedMilliseconds = timer.elapsed();

    QCOMPARE(repository.SyncCursor(QStringLiteral("alice")), finalCursor);
    const auto restored = repository.LoadScenario(QStringLiteral("local-sqlite"));
    int restoredBatchCount = 0;
    for (const auto& message : restored.messagesByConversation.value(QStringLiteral("alice"))) {
        if (message.messageId.has_value() && *message.messageId >= 200000 &&
            *message.messageId < 200000 + kMessageCount) {
            ++restoredBatchCount;
        }
    }
    QCOMPARE(restoredBatchCount, kMessageCount);
    QVERIFY2(elapsedMilliseconds < 5000,
             qPrintable(QStringLiteral("two 2000-message sync batches took %1 ms").arg(elapsedMilliseconds)));
}

void ClientModelsTest::sqliteLoadsMessagesByConversationSequence()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SqliteClientRepository repository(directory.filePath(QStringLiteral("ordered.sqlite")), false);
    QVERIFY2(repository.IsReady(), qPrintable(repository.LastError()));
    QVERIFY(repository.SaveConversation(ConversationRecord{
        .conversationId = QStringLiteral("direct:42"),
        .title = QStringLiteral("Alice"),
        .avatarColor = QStringLiteral("#315FD6"),
        .remoteConversationId = 9001,
    }));

    QVector<MessageRecord> reversed;
    for (const std::int64_t sequence : {3, 1, 2}) {
        MessageRecord message{
            .clientMessageId = 7000 + sequence,
            .messageId = 7000 + sequence,
            .conversationSeq = sequence,
            .senderId = QStringLiteral("42"),
            .body = QStringLiteral("message-%1").arg(sequence),
            .timestamp = QStringLiteral("14:00"),
            .outgoing = false,
            .deliveryState = MessageDeliveryState::Delivered,
        };
        if (sequence == 3) {
            message.body = QStringLiteral("3d95263b-7d19-4aa3-9c94-6e6e5d15e58d");
            message.type = 5;
            message.fileId = message.body;
            message.fileName = QStringLiteral("notes.md");
            message.fileType = QStringLiteral("text/markdown");
            message.fileSize = 17;
            message.fileExpired = true;
        }
        reversed.push_back(std::move(message));
    }
    QVERIFY(repository.ApplyIncomingBatch(QStringLiteral("direct:42"), reversed, 3));

    const auto restored = repository.LoadScenario(QStringLiteral("local-sqlite"));
    const auto messages = restored.messagesByConversation.value(QStringLiteral("direct:42"));
    QCOMPARE(messages.size(), 3);
    QCOMPARE(messages[0].conversationSeq, std::optional<std::int64_t>(1));
    QCOMPARE(messages[1].conversationSeq, std::optional<std::int64_t>(2));
    QCOMPARE(messages[2].conversationSeq, std::optional<std::int64_t>(3));
    QCOMPARE(messages[2].body, messages[2].fileId);
    QCOMPARE(messages[2].fileName, QStringLiteral("notes.md"));
    QCOMPARE(messages[2].fileType, QStringLiteral("text/markdown"));
    QCOMPARE(messages[2].fileSize, 17);
    QVERIFY(messages[2].fileExpired);
}

void ClientModelsTest::sqliteArchivesConversationWithoutDeletingHistory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("group-history.sqlite"));
    {
        SqliteClientRepository repository(databasePath, false);
        QVERIFY2(repository.IsReady(), qPrintable(repository.LastError()));
        QVERIFY(repository.SaveConversation(ConversationRecord{
            .conversationId = QStringLiteral("group:5001"),
            .title = QStringLiteral("群聊 5001"),
            .avatarColor = QStringLiteral("#7656A8"),
            .remoteConversationId = 9501,
        }));
        QVERIFY(repository.ApplyIncomingBatch(QStringLiteral("group:5001"),
                                              {MessageRecord{
                                                  .clientMessageId = 6001,
                                                  .messageId = 7001,
                                                  .conversationSeq = 1,
                                                  .senderId = QStringLiteral("42"),
                                                  .body = QStringLiteral("保留的群消息"),
                                                  .timestamp = QStringLiteral("14:00"),
                                                  .outgoing = false,
                                                  .deliveryState = MessageDeliveryState::Delivered,
                                              }},
                                              1));
        QVERIFY(repository.RemoveConversation(QStringLiteral("group:5001")));
        QCOMPARE(repository.LoadScenario(QStringLiteral("local-sqlite")).conversations.size(), 0);
    }

    const QString connectionName = TestConnectionName();
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(databasePath);
    QVERIFY(database.open());
    QSqlQuery query(database);
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM messages WHERE conversation_id = 'group:5001'")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    database.close();
    database = {};
    QSqlDatabase::removeDatabase(connectionName);
}

void ClientModelsTest::sqlitePersistsNetworkProjection()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString databasePath = directory.filePath(QStringLiteral("network-projection.sqlite"));

    {
        SqliteClientRepository repository(databasePath, false);
        QVERIFY2(repository.IsReady(), qPrintable(repository.LastError()));
        QVERIFY2(repository.SaveConversation(ConversationRecord{
                     .conversationId = QStringLiteral("direct:42"),
                     .title = QStringLiteral("服务端用户"),
                     .preview = QString{},
                     .timestamp = QString{},
                     .avatarColor = QStringLiteral("#315FD6"),
                     .remoteConversationId = 9001,
                 }),
                 qPrintable(repository.LastError()));
        QVERIFY(repository.ReplaceContacts({ContactRecord{
            .userId = QStringLiteral("42"),
            .displayName = QStringLiteral("服务端用户"),
            .statusText = QStringLiteral("WIMI 用户"),
            .avatarColor = QStringLiteral("#315FD6"),
        }}));
        QVERIFY(repository.ReplaceRequests({RequestRecord{
            .requestId = QStringLiteral("43"),
            .displayName = QStringLiteral("用户 43"),
            .message = QStringLiteral("hello"),
            .avatarColor = QStringLiteral("#367A91"),
            .kind = QStringLiteral("friend"),
            .status = QStringLiteral("pending"),
        }}));
    }

    SqliteClientRepository restoredRepository(databasePath, false);
    QVERIFY2(restoredRepository.IsReady(), qPrintable(restoredRepository.LastError()));
    const auto restored = restoredRepository.LoadScenario(QStringLiteral("local-sqlite"));
    QCOMPARE(restored.conversations.size(), 1);
    QCOMPARE(restored.conversations.front().conversationId, QStringLiteral("direct:42"));
    QCOMPARE(restored.conversations.front().remoteConversationId, std::optional<std::int64_t>(9001));
    QCOMPARE(restored.contacts.size(), 1);
    QCOMPARE(restored.contacts.front().userId, QStringLiteral("42"));
    QCOMPARE(restored.requests.size(), 1);
    QCOMPARE(restored.requests.front().status, QStringLiteral("pending"));
    QVERIFY(SqliteIndexExists(databasePath, QStringLiteral("idx_conversations_remote_id")));
}

void ClientModelsTest::platformNotificationPortUpdatesControllerState()
{
    auto platformServices = std::make_unique<RecordingPlatformServices>();
    auto* recordingServices = platformServices.get();
    AppController controller(std::move(platformServices));
    QSignalSpy statusSpy(&controller, &AppController::notificationTestStatusChanged);

    QCOMPARE(controller.platformName(), QStringLiteral("test-linux"));
    QVERIFY(controller.desktopNotificationsAvailable());
    QCOMPARE(controller.notificationTestStatus(), QStringLiteral("idle"));

    controller.sendTestDesktopNotification();
    QCOMPARE(recordingServices->notificationCount, 1);
    QVERIFY(!recordingServices->lastTitle.isEmpty());
    QVERIFY(!recordingServices->lastBody.isEmpty());
    QCOMPARE(controller.notificationTestStatus(), QStringLiteral("sent"));
    QCOMPARE(statusSpy.count(), 1);

    recordingServices->showResult = false;
    controller.sendTestDesktopNotification();
    QCOMPARE(recordingServices->notificationCount, 2);
    QCOMPARE(controller.notificationTestStatus(), QStringLiteral("unavailable"));
    QCOMPARE(statusSpy.count(), 2);
}

void ClientModelsTest::tcpFrameCodecHandlesFragmentationAndCoalescing()
{
    const QByteArray first = TcpFrameCodec::Encode(QByteArrayLiteral("login"));
    const QByteArray second = TcpFrameCodec::Encode(QByteArrayLiteral("message"));

    QCOMPARE(first.left(4).toHex(), QByteArrayLiteral("00000005"));

    TcpFrameCodec codec;
    QVERIFY(codec.Feed(first.left(3)).isEmpty());
    const auto frames = codec.Feed(first.mid(3) + second);
    QCOMPARE(frames.size(), 2);
    QCOMPARE(frames[0].payload, QByteArrayLiteral("login"));
    QCOMPARE(frames[1].payload, QByteArrayLiteral("message"));
    QVERIFY(!codec.HasError());
}

void ClientModelsTest::tcpFrameCodecRejectsOversizedPayload()
{
    QByteArray header;
    QDataStream stream(&header, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << quint32(TcpFrameCodec::kMaximumPayloadSize + 1);

    TcpFrameCodec codec;
    QVERIFY(codec.Feed(header).isEmpty());
    QVERIFY(codec.HasError());
    QVERIFY(codec.ErrorString().contains(QStringLiteral("10 MiB")));
}

} // namespace wimi::client

QTEST_GUILESS_MAIN(wimi::client::ClientModelsTest)

#include "ClientModelsTest.moc"
