#include "adapters/sqlite/SqliteClientRepository.h"

#include "adapters/fake/FakeScenarioRepository.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace wimi::client
{
namespace
{

int DeliveryStateValue(MessageDeliveryState state) { return static_cast<int>(state); }

MessageDeliveryState DeliveryStateFromValue(int value)
{
    if (value < DeliveryStateValue(MessageDeliveryState::PendingLocal) ||
        value > DeliveryStateValue(MessageDeliveryState::PermanentFailed)) {
        return MessageDeliveryState::Unknown;
    }
    return static_cast<MessageDeliveryState>(value);
}

bool Execute(QSqlQuery& query, const QString& statement, QString* error)
{
    if (query.exec(statement)) {
        return true;
    }
    if (error != nullptr) {
        *error = query.lastError().text();
    }
    return false;
}

bool ColumnExists(QSqlDatabase& database, const QString& table, const QString& column)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        return false;
    }
    while (query.next()) {
        if (query.value(1).toString() == column) {
            return true;
        }
    }
    return false;
}

} // namespace

SqliteClientRepository::SqliteClientRepository(QString databasePath, bool seedDemoData)
    : database_path_(std::move(databasePath)),
      connection_name_(QStringLiteral("wimi-client-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))),
      seed_demo_data_(seedDemoData)
{
    Open();
}

SqliteClientRepository::~SqliteClientRepository()
{
    if (database_.isValid()) {
        database_.close();
    }
    database_ = {};
    QSqlDatabase::removeDatabase(connection_name_);
}

QStringList SqliteClientRepository::ScenarioNames() const { return {QStringLiteral("local-sqlite")}; }

ClientSnapshot SqliteClientRepository::LoadScenario(const QString&) const
{
    ClientSnapshot snapshot{
        .scenarioName = QStringLiteral("local-sqlite"),
        .connectionStatus = IsReady() ? QStringLiteral("offline-cached") : QStringLiteral("storage-error"),
    };
    if (!IsReady()) {
        return snapshot;
    }

    QSqlQuery conversationsQuery(database_);
    if (!conversationsQuery.exec(QStringLiteral("SELECT conversation_id, title, preview, timestamp, avatar_color, "
                                                "remote_conversation_id, unread_count, pinned, muted, online, "
                                                "draft_text "
                                                "FROM conversations ORDER BY pinned DESC, sort_order ASC"))) {
        SetError(conversationsQuery.lastError().text());
        return snapshot;
    }

    while (conversationsQuery.next()) {
        const QString conversationId = conversationsQuery.value(0).toString();
        ConversationRecord conversation{
            .conversationId = conversationId,
            .title = conversationsQuery.value(1).toString(),
            .preview = conversationsQuery.value(2).toString(),
            .timestamp = conversationsQuery.value(3).toString(),
            .avatarColor = conversationsQuery.value(4).toString(),
            .unreadCount = conversationsQuery.value(6).toInt(),
            .pinned = conversationsQuery.value(7).toBool(),
            .muted = conversationsQuery.value(8).toBool(),
            .online = conversationsQuery.value(9).toBool(),
        };
        if (!conversationsQuery.value(5).isNull()) {
            conversation.remoteConversationId = conversationsQuery.value(5).toLongLong();
        }
        snapshot.conversations.push_back(std::move(conversation));
        snapshot.draftsByConversation.insert(conversationId, conversationsQuery.value(10).toString());
    }

    QSqlQuery messagesQuery(database_);
    if (messagesQuery.exec(QStringLiteral("SELECT conversation_id, client_message_id, message_id, "
                                          "conversation_seq, sender_id, body, type, file_id, file_name, file_type, "
                                          "file_size, file_expired, timestamp, outgoing, "
                                          "delivery_state FROM messages "
                                          "ORDER BY conversation_id, conversation_seq IS NULL, "
                                          "conversation_seq ASC, local_order ASC"))) {
        while (messagesQuery.next()) {
            MessageRecord message{
                .clientMessageId = messagesQuery.value(1).toLongLong(),
                .senderId = messagesQuery.value(4).toString(),
                .body = messagesQuery.value(5).toString(),
                .type = messagesQuery.value(6).toInt(),
                .fileId = messagesQuery.value(7).toString(),
                .fileName = messagesQuery.value(8).toString(),
                .fileType = messagesQuery.value(9).toString(),
                .fileSize = messagesQuery.value(10).toLongLong(),
                .fileExpired = messagesQuery.value(11).toBool(),
                .timestamp = messagesQuery.value(12).toString(),
                .outgoing = messagesQuery.value(13).toBool(),
                .deliveryState = DeliveryStateFromValue(messagesQuery.value(14).toInt()),
            };
            if (!messagesQuery.value(2).isNull()) {
                message.messageId = messagesQuery.value(2).toLongLong();
            }
            if (!messagesQuery.value(3).isNull()) {
                message.conversationSeq = messagesQuery.value(3).toLongLong();
            }
            snapshot.messagesByConversation[messagesQuery.value(0).toString()].push_back(std::move(message));
        }
    } else {
        SetError(messagesQuery.lastError().text());
    }

    QSqlQuery contactsQuery(database_);
    if (contactsQuery.exec(QStringLiteral("SELECT user_id, display_name, status_text, avatar_color, online, "
                                          "favorite FROM contacts ORDER BY favorite DESC, display_name"))) {
        while (contactsQuery.next()) {
            snapshot.contacts.push_back(ContactRecord{
                .userId = contactsQuery.value(0).toString(),
                .displayName = contactsQuery.value(1).toString(),
                .statusText = contactsQuery.value(2).toString(),
                .avatarColor = contactsQuery.value(3).toString(),
                .online = contactsQuery.value(4).toBool(),
                .favorite = contactsQuery.value(5).toBool(),
            });
        }
    }

    QSqlQuery requestsQuery(database_);
    if (requestsQuery.exec(QStringLiteral("SELECT request_id, display_name, message, avatar_color, kind, "
                                          "status FROM requests ORDER BY local_order"))) {
        while (requestsQuery.next()) {
            snapshot.requests.push_back(RequestRecord{
                .requestId = requestsQuery.value(0).toString(),
                .displayName = requestsQuery.value(1).toString(),
                .message = requestsQuery.value(2).toString(),
                .avatarColor = requestsQuery.value(3).toString(),
                .kind = requestsQuery.value(4).toString(),
                .status = requestsQuery.value(5).toString(),
            });
        }
    }

    return snapshot;
}

QString SqliteClientRepository::RepositoryKind() const { return QStringLiteral("sqlite"); }

bool SqliteClientRepository::EnqueueOutgoing(const QString& conversationId, const MessageRecord& message)
{
    if (!IsReady() || !database_.transaction()) {
        SetError(database_.lastError().text());
        return false;
    }

    if (!InsertMessage(conversationId, message, false)) {
        database_.rollback();
        return false;
    }

    QSqlQuery outboxQuery(database_);
    outboxQuery.prepare(QStringLiteral("INSERT INTO outbox(client_message_id, created_at, attempt_count) "
                                       "VALUES(?, strftime('%s','now'), 0)"));
    outboxQuery.addBindValue(QVariant::fromValue<qlonglong>(message.clientMessageId));
    if (!outboxQuery.exec()) {
        SetError(outboxQuery.lastError().text());
        database_.rollback();
        return false;
    }

    QSqlQuery conversationQuery(database_);
    conversationQuery.prepare(QStringLiteral("UPDATE conversations SET preview = ?, timestamp = ? "
                                             "WHERE conversation_id = ?"));
    conversationQuery.addBindValue(message.body);
    conversationQuery.addBindValue(message.timestamp);
    conversationQuery.addBindValue(conversationId);
    if (!conversationQuery.exec() || !database_.commit()) {
        SetError(conversationQuery.lastError().text().isEmpty() ? database_.lastError().text()
                                                                : conversationQuery.lastError().text());
        database_.rollback();
        return false;
    }
    return true;
}

bool SqliteClientRepository::UpdateDeliveryState(std::int64_t clientMessageId, MessageDeliveryState state)
{
    if (!IsReady() || !database_.transaction()) {
        return false;
    }

    QSqlQuery updateQuery(database_);
    updateQuery.prepare(QStringLiteral("UPDATE messages SET delivery_state = ? WHERE client_message_id = ?"));
    updateQuery.addBindValue(DeliveryStateValue(state));
    updateQuery.addBindValue(QVariant::fromValue<qlonglong>(clientMessageId));
    if (!updateQuery.exec()) {
        SetError(updateQuery.lastError().text());
        database_.rollback();
        return false;
    }

    if (state == MessageDeliveryState::Accepted || state == MessageDeliveryState::Delivered ||
        state == MessageDeliveryState::Read || state == MessageDeliveryState::PermanentFailed) {
        QSqlQuery deleteQuery(database_);
        deleteQuery.prepare(QStringLiteral("DELETE FROM outbox WHERE client_message_id = ?"));
        deleteQuery.addBindValue(QVariant::fromValue<qlonglong>(clientMessageId));
        if (!deleteQuery.exec()) {
            SetError(deleteQuery.lastError().text());
            database_.rollback();
            return false;
        }
    }

    if (!database_.commit()) {
        SetError(database_.lastError().text());
        return false;
    }
    return true;
}

bool SqliteClientRepository::AcceptOutgoing(std::int64_t clientMessageId, std::int64_t messageId,
                                            const QString& conversationId, std::int64_t conversationSeq)
{
    if (!IsReady() || !database_.transaction()) {
        return false;
    }

    QSqlQuery updateQuery(database_);
    updateQuery.prepare(QStringLiteral("UPDATE messages SET message_id = ?, conversation_seq = ?, "
                                       "delivery_state = ? WHERE client_message_id = ?"));
    updateQuery.addBindValue(QVariant::fromValue<qlonglong>(messageId));
    updateQuery.addBindValue(QVariant::fromValue<qlonglong>(conversationSeq));
    updateQuery.addBindValue(DeliveryStateValue(MessageDeliveryState::Accepted));
    updateQuery.addBindValue(QVariant::fromValue<qlonglong>(clientMessageId));
    if (!updateQuery.exec()) {
        SetError(updateQuery.lastError().text());
        database_.rollback();
        return false;
    }

    QSqlQuery deleteQuery(database_);
    deleteQuery.prepare(QStringLiteral("DELETE FROM outbox WHERE client_message_id = ?"));
    deleteQuery.addBindValue(QVariant::fromValue<qlonglong>(clientMessageId));
    if (!deleteQuery.exec() || !database_.commit()) {
        SetError(deleteQuery.lastError().text().isEmpty() ? database_.lastError().text()
                                                          : deleteQuery.lastError().text());
        database_.rollback();
        return false;
    }
    return true;
}

bool SqliteClientRepository::StoreDraft(const QString& conversationId, const QString& draft)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE conversations SET draft_text = ? WHERE conversation_id = ?"));
    query.addBindValue(draft);
    query.addBindValue(conversationId);
    if (!query.exec()) {
        SetError(query.lastError().text());
        return false;
    }
    return true;
}

bool SqliteClientRepository::SaveConversation(const ConversationRecord& conversation)
{
    if (!IsReady()) {
        return false;
    }
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT INTO conversations(conversation_id, title, preview, timestamp, "
                                 "avatar_color, remote_conversation_id, unread_count, pinned, muted, "
                                 "online, sort_order) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                                 "(SELECT COALESCE(MIN(sort_order), 0) - 1 FROM conversations)) "
                                 "ON CONFLICT(conversation_id) DO UPDATE SET title = excluded.title, "
                                 "preview = excluded.preview, timestamp = excluded.timestamp, "
                                 "avatar_color = excluded.avatar_color, remote_conversation_id = "
                                 "COALESCE(excluded.remote_conversation_id, remote_conversation_id), "
                                 "unread_count = excluded.unread_count, pinned = excluded.pinned, "
                                 "muted = excluded.muted, online = excluded.online"));
    query.addBindValue(conversation.conversationId);
    query.addBindValue(conversation.title.isNull() ? QStringLiteral("") : conversation.title);
    query.addBindValue(conversation.preview.isNull() ? QStringLiteral("") : conversation.preview);
    query.addBindValue(conversation.timestamp.isNull() ? QStringLiteral("") : conversation.timestamp);
    query.addBindValue(conversation.avatarColor.isNull() ? QStringLiteral("#315FD6") : conversation.avatarColor);
    query.addBindValue(conversation.remoteConversationId.has_value()
                           ? QVariant::fromValue<qlonglong>(*conversation.remoteConversationId)
                           : QVariant{});
    query.addBindValue(conversation.unreadCount);
    query.addBindValue(conversation.pinned);
    query.addBindValue(conversation.muted);
    query.addBindValue(conversation.online);
    if (!query.exec()) {
        SetError(query.lastError().text());
        return false;
    }
    return true;
}

bool SqliteClientRepository::SetRemoteConversationId(const QString& conversationId, std::int64_t remoteConversationId)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE conversations SET remote_conversation_id = ? "
                                 "WHERE conversation_id = ?"));
    query.addBindValue(QVariant::fromValue<qlonglong>(remoteConversationId));
    query.addBindValue(conversationId);
    if (!query.exec()) {
        SetError(query.lastError().text());
        return false;
    }
    return query.numRowsAffected() == 1;
}

bool SqliteClientRepository::ReplaceContacts(const QVector<ContactRecord>& contacts)
{
    if (!IsReady() || !database_.transaction()) {
        return false;
    }
    QSqlQuery clear(database_);
    if (!clear.exec(QStringLiteral("DELETE FROM contacts"))) {
        SetError(clear.lastError().text());
        database_.rollback();
        return false;
    }
    for (const auto& contact : contacts) {
        QSqlQuery query(database_);
        query.prepare(QStringLiteral("INSERT INTO contacts(user_id, display_name, status_text, "
                                     "avatar_color, online, favorite) VALUES(?, ?, ?, ?, ?, ?)"));
        query.addBindValue(contact.userId);
        query.addBindValue(contact.displayName);
        query.addBindValue(contact.statusText);
        query.addBindValue(contact.avatarColor);
        query.addBindValue(contact.online);
        query.addBindValue(contact.favorite);
        if (!query.exec()) {
            SetError(query.lastError().text());
            database_.rollback();
            return false;
        }
    }
    if (!database_.commit()) {
        SetError(database_.lastError().text());
        return false;
    }
    return true;
}

bool SqliteClientRepository::ReplaceRequests(const QVector<RequestRecord>& requests)
{
    if (!IsReady() || !database_.transaction()) {
        return false;
    }
    QSqlQuery clear(database_);
    if (!clear.exec(QStringLiteral("DELETE FROM requests"))) {
        SetError(clear.lastError().text());
        database_.rollback();
        return false;
    }
    int order = 0;
    for (const auto& request : requests) {
        QSqlQuery query(database_);
        query.prepare(QStringLiteral("INSERT INTO requests(request_id, display_name, message, avatar_color, "
                                     "kind, status, local_order) VALUES(?, ?, ?, ?, ?, ?, ?)"));
        query.addBindValue(request.requestId);
        query.addBindValue(request.displayName);
        query.addBindValue(request.message);
        query.addBindValue(request.avatarColor);
        query.addBindValue(request.kind);
        query.addBindValue(request.status);
        query.addBindValue(order++);
        if (!query.exec()) {
            SetError(query.lastError().text());
            database_.rollback();
            return false;
        }
    }
    if (!database_.commit()) {
        SetError(database_.lastError().text());
        return false;
    }
    return true;
}

bool SqliteClientRepository::IsReady() const { return database_.isValid() && database_.isOpen() && ready_; }

QString SqliteClientRepository::LastError() const { return last_error_; }

QString SqliteClientRepository::DatabasePath() const { return database_path_; }

bool SqliteClientRepository::ApplyIncomingBatch(const QString& conversationId, const QVector<MessageRecord>& messages,
                                                std::int64_t nextCursor)
{
    if (!IsReady() || !database_.transaction()) {
        return false;
    }

    for (const auto& message : messages) {
        if (!InsertMessage(conversationId, message, true)) {
            database_.rollback();
            return false;
        }
    }

    QSqlQuery cursorQuery(database_);
    cursorQuery.prepare(QStringLiteral("INSERT INTO sync_state(scope, cursor, updated_at) "
                                       "VALUES(?, ?, strftime('%s','now')) "
                                       "ON CONFLICT(scope) DO UPDATE SET cursor = excluded.cursor, "
                                       "updated_at = excluded.updated_at "
                                       "WHERE excluded.cursor >= sync_state.cursor"));
    cursorQuery.addBindValue(QStringLiteral("conversation:") + conversationId);
    cursorQuery.addBindValue(QVariant::fromValue<qlonglong>(nextCursor));
    if (!cursorQuery.exec() || !database_.commit()) {
        SetError(cursorQuery.lastError().text().isEmpty() ? database_.lastError().text()
                                                          : cursorQuery.lastError().text());
        database_.rollback();
        return false;
    }
    return true;
}

std::int64_t SqliteClientRepository::SyncCursor(const QString& conversationId) const
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT cursor FROM sync_state WHERE scope = ?"));
    query.addBindValue(QStringLiteral("conversation:") + conversationId);
    if (!query.exec() || !query.next()) {
        return 0;
    }
    return query.value(0).toLongLong();
}

int SqliteClientRepository::OutboxCount() const
{
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM outbox")) || !query.next()) {
        return -1;
    }
    return query.value(0).toInt();
}

QVector<FileUploadRecord> SqliteClientRepository::LoadFileUploads() const
{
    QVector<FileUploadRecord> uploads;
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT file_id, source_path, file_name, file_type, file_size, next_offset, "
                                   "conversation_id, upload_complete FROM file_uploads ORDER BY updated_at")))
        return uploads;
    while (query.next()) {
        uploads.push_back(FileUploadRecord{.fileId = query.value(0).toString(),
                                           .sourcePath = query.value(1).toString(),
                                           .fileName = query.value(2).toString(),
                                           .fileType = query.value(3).toString(),
                                           .fileSize = query.value(4).toLongLong(),
                                           .nextOffset = query.value(5).toLongLong(),
                                           .conversationId = query.value(6).toString(),
                                           .uploadComplete = query.value(7).toBool()});
    }
    return uploads;
}

bool SqliteClientRepository::SaveFileUpload(const FileUploadRecord& upload)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT INTO file_uploads(file_id, source_path, file_name, file_type, file_size, "
                                 "next_offset, conversation_id, upload_complete, updated_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?, "
                                 "strftime('%s','now')) ON CONFLICT(file_id) DO UPDATE SET source_path=excluded.source_path, "
                                 "file_name=excluded.file_name,file_type=excluded.file_type,file_size=excluded.file_size,"
                                 "next_offset=excluded.next_offset,conversation_id=excluded.conversation_id,"
                                 "upload_complete=excluded.upload_complete,updated_at=excluded.updated_at"));
    query.addBindValue(upload.fileId);
    query.addBindValue(upload.sourcePath);
    query.addBindValue(upload.fileName);
    query.addBindValue(upload.fileType);
    query.addBindValue(QVariant::fromValue<qlonglong>(upload.fileSize));
    query.addBindValue(QVariant::fromValue<qlonglong>(upload.nextOffset));
    query.addBindValue(upload.conversationId);
    query.addBindValue(upload.uploadComplete);
    return query.exec();
}

bool SqliteClientRepository::RemoveFileUpload(const QString& fileId)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("DELETE FROM file_uploads WHERE file_id = ?"));
    query.addBindValue(fileId);
    return query.exec();
}

QVector<FileDownloadRecord> SqliteClientRepository::LoadFileDownloads() const
{
    QVector<FileDownloadRecord> downloads;
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT file_id, file_type, sha256, file_size, next_offset, temporary_path FROM file_downloads")))
        return downloads;
    while (query.next()) downloads.push_back(FileDownloadRecord{.fileId = query.value(0).toString(),
        .fileType = query.value(1).toString(), .sha256 = query.value(2).toByteArray(),
        .fileSize = query.value(3).toLongLong(), .nextOffset = query.value(4).toLongLong(),
        .temporaryPath = query.value(5).toString()});
    return downloads;
}

bool SqliteClientRepository::SaveFileDownload(const FileDownloadRecord& download)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT INTO file_downloads(file_id,file_type,sha256,file_size,next_offset,temporary_path,updated_at) "
                                 "VALUES(?,?,?,?,?,?,strftime('%s','now')) ON CONFLICT(file_id) DO UPDATE SET "
                                 "file_type=excluded.file_type,sha256=excluded.sha256,file_size=excluded.file_size,"
                                 "next_offset=excluded.next_offset,temporary_path=excluded.temporary_path,updated_at=excluded.updated_at"));
    query.addBindValue(download.fileId); query.addBindValue(download.fileType);
    query.addBindValue(download.sha256); query.addBindValue(QVariant::fromValue<qlonglong>(download.fileSize));
    query.addBindValue(QVariant::fromValue<qlonglong>(download.nextOffset)); query.addBindValue(download.temporaryPath);
    return query.exec();
}

bool SqliteClientRepository::RemoveFileDownload(const QString& fileId)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("DELETE FROM file_downloads WHERE file_id=?")); query.addBindValue(fileId);
    return query.exec();
}

bool SqliteClientRepository::Open()
{
    if (database_path_ != QStringLiteral(":memory:")) {
        const QFileInfo fileInfo(database_path_);
        if (!QDir().mkpath(fileInfo.absolutePath())) {
            SetError(QStringLiteral("cannot create database directory"));
            return false;
        }
    }

    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_name_);
    database_.setDatabaseName(database_path_);
    if (!database_.open()) {
        SetError(database_.lastError().text());
        return false;
    }

    QSqlQuery pragmaQuery(database_);
    QString error;
    if (!Execute(pragmaQuery, QStringLiteral("PRAGMA foreign_keys = ON"), &error) ||
        !Execute(pragmaQuery, QStringLiteral("PRAGMA journal_mode = WAL"), &error) ||
        !Execute(pragmaQuery, QStringLiteral("PRAGMA busy_timeout = 5000"), &error)) {
        SetError(error);
        return false;
    }
    ready_ = Migrate() && (!seed_demo_data_ || SeedIfEmpty());
    return ready_;
}

bool SqliteClientRepository::Migrate()
{
    QSqlQuery versionQuery(database_);
    if (!versionQuery.exec(QStringLiteral("PRAGMA user_version")) || !versionQuery.next()) {
        SetError(versionQuery.lastError().text().isEmpty() ? QStringLiteral("cannot read SQLite schema version")
                                                           : versionQuery.lastError().text());
        return false;
    }

    int schemaVersion = versionQuery.value(0).toInt();
    if (schemaVersion < 0) {
        SetError(QStringLiteral("invalid negative SQLite schema version"));
        return false;
    }
    if (schemaVersion > kCurrentSchemaVersion) {
        SetError(QStringLiteral("SQLite schema version %1 is newer than supported "
                                "version %2")
                     .arg(schemaVersion)
                     .arg(kCurrentSchemaVersion));
        return false;
    }

    if (!database_.transaction()) {
        SetError(database_.lastError().text());
        return false;
    }

    const QStringList versionOneStatements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS conversations("
                       "conversation_id TEXT PRIMARY KEY, title TEXT NOT NULL, "
                       "preview TEXT NOT NULL DEFAULT '', timestamp TEXT NOT "
                       "NULL DEFAULT '', "
                       "avatar_color TEXT NOT NULL, unread_count INTEGER NOT "
                       "NULL DEFAULT 0, "
                       "pinned INTEGER NOT NULL DEFAULT 0, muted INTEGER NOT "
                       "NULL DEFAULT 0, "
                       "online INTEGER NOT NULL DEFAULT 0, draft_text TEXT NOT "
                       "NULL DEFAULT '', "
                       "sort_order INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS messages("
                       "local_order INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "client_message_id INTEGER NOT NULL UNIQUE, message_id "
                       "INTEGER UNIQUE, "
                       "conversation_id TEXT NOT NULL REFERENCES "
                       "conversations(conversation_id) "
                       "ON DELETE CASCADE, conversation_seq INTEGER, sender_id "
                       "TEXT NOT NULL, "
                       "body TEXT NOT NULL, type INTEGER NOT NULL DEFAULT 1, file_id TEXT NOT NULL DEFAULT '', "
                       "file_name TEXT NOT NULL DEFAULT '', file_type TEXT NOT NULL DEFAULT '', "
                       "file_size INTEGER NOT NULL DEFAULT 0, file_expired INTEGER NOT NULL DEFAULT 0, "
                       "timestamp TEXT NOT NULL, outgoing "
                       "INTEGER NOT NULL, "
                       "delivery_state INTEGER NOT NULL, "
                       "UNIQUE(conversation_id, conversation_seq))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS outbox("
                       "client_message_id INTEGER PRIMARY KEY REFERENCES "
                       "messages(client_message_id) "
                       "ON DELETE CASCADE, created_at INTEGER NOT NULL, "
                       "attempt_count INTEGER NOT NULL DEFAULT 0, next_retry_at INTEGER)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS sync_state("
                       "scope TEXT PRIMARY KEY, cursor INTEGER NOT NULL, "
                       "updated_at INTEGER NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS contacts("
                       "user_id TEXT PRIMARY KEY, display_name TEXT NOT NULL, "
                       "status_text TEXT NOT NULL, avatar_color TEXT NOT NULL, "
                       "online INTEGER NOT NULL DEFAULT 0, favorite INTEGER NOT "
                       "NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS requests("
                       "request_id TEXT PRIMARY KEY, display_name TEXT NOT NULL, "
                       "message TEXT NOT NULL, avatar_color TEXT NOT NULL, kind TEXT NOT "
                       "NULL, "
                       "status TEXT NOT NULL, local_order INTEGER NOT NULL DEFAULT 0)"),
    };

    const QStringList versionTwoStatements = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_messages_conversation_local_order "
                       "ON messages(conversation_id, local_order)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_outbox_retry "
                       "ON outbox(next_retry_at, created_at)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_conversations_sort "
                       "ON conversations(pinned DESC, sort_order ASC)"),
    };

    QStringList versionThreeStatements;
    if (!ColumnExists(database_, QStringLiteral("conversations"), QStringLiteral("remote_conversation_id"))) {
        versionThreeStatements.push_back(
            QStringLiteral("ALTER TABLE conversations ADD COLUMN remote_conversation_id INTEGER"));
    }
    versionThreeStatements.push_back(
        QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS "
                       "idx_conversations_remote_id ON conversations(remote_conversation_id) "
                       "WHERE remote_conversation_id IS NOT NULL"));

    const auto applyMigration = [this](int targetVersion, const QStringList& statements) {
        for (const auto& statement : statements) {
            QSqlQuery query(database_);
            if (!query.exec(statement)) {
                SetError(query.lastError().text());
                return false;
            }
        }

        QSqlQuery versionUpdate(database_);
        if (!versionUpdate.exec(QStringLiteral("PRAGMA user_version = %1").arg(targetVersion))) {
            SetError(versionUpdate.lastError().text());
            return false;
        }
        return true;
    };

    if (schemaVersion < 1) {
        if (!applyMigration(1, versionOneStatements)) {
            database_.rollback();
            return false;
        }
        schemaVersion = 1;
    }
    if (schemaVersion < 2) {
        if (!applyMigration(2, versionTwoStatements)) {
            database_.rollback();
            return false;
        }
        schemaVersion = 2;
    }
    if (schemaVersion < 3 && !applyMigration(3, versionThreeStatements)) {
        database_.rollback();
        return false;
    }
    QStringList versionFourStatements;
    if (!ColumnExists(database_, QStringLiteral("messages"), QStringLiteral("type"))) {
        versionFourStatements.push_back(QStringLiteral("ALTER TABLE messages ADD COLUMN type INTEGER NOT NULL DEFAULT 1"));
    }
    if (schemaVersion < 4 && !applyMigration(4, versionFourStatements)) {
        database_.rollback();
        return false;
    }
    const QStringList versionFiveStatements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS file_uploads("
                       "file_id TEXT PRIMARY KEY, source_path TEXT NOT NULL, file_name TEXT NOT NULL, "
                       "file_type TEXT NOT NULL, file_size INTEGER NOT NULL, next_offset INTEGER NOT NULL, "
                       "conversation_id TEXT NOT NULL, updated_at INTEGER NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_file_uploads_updated ON file_uploads(updated_at)"),
    };
    if (schemaVersion < 5 && !applyMigration(5, versionFiveStatements)) {
        database_.rollback();
        return false;
    }
    QStringList versionSixStatements;
    if (!ColumnExists(database_, QStringLiteral("messages"), QStringLiteral("file_id"))) {
        versionSixStatements.push_back(QStringLiteral("ALTER TABLE messages ADD COLUMN file_id TEXT NOT NULL DEFAULT ''"));
    }
    if (schemaVersion < 6 && !applyMigration(6, versionSixStatements)) {
        database_.rollback();
        return false;
    }
    const QStringList versionSevenStatements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS file_downloads(file_id TEXT PRIMARY KEY, file_type TEXT NOT NULL, "
                       "file_size INTEGER NOT NULL, next_offset INTEGER NOT NULL, temporary_path TEXT NOT NULL, "
                       "updated_at INTEGER NOT NULL)"),
    };
    if (schemaVersion < 7 && !applyMigration(7, versionSevenStatements)) {
        database_.rollback();
        return false;
    }
    QStringList versionEightStatements;
    if (!ColumnExists(database_, QStringLiteral("file_downloads"), QStringLiteral("sha256"))) {
        versionEightStatements.push_back(
            QStringLiteral("ALTER TABLE file_downloads ADD COLUMN sha256 BLOB NOT NULL DEFAULT X''"));
    }
    if (schemaVersion < 8 && !applyMigration(8, versionEightStatements)) {
        database_.rollback();
        return false;
    }
    QStringList versionNineStatements;
    if (!ColumnExists(database_, QStringLiteral("file_uploads"), QStringLiteral("upload_complete"))) {
        versionNineStatements.push_back(
            QStringLiteral("ALTER TABLE file_uploads ADD COLUMN upload_complete INTEGER NOT NULL DEFAULT 0"));
    }
    if (schemaVersion < 9 && !applyMigration(9, versionNineStatements)) {
        database_.rollback();
        return false;
    }
    QStringList versionTenStatements;
    if (!ColumnExists(database_, QStringLiteral("messages"), QStringLiteral("file_name"))) {
        versionTenStatements.push_back(
            QStringLiteral("ALTER TABLE messages ADD COLUMN file_name TEXT NOT NULL DEFAULT ''"));
    }
    if (!ColumnExists(database_, QStringLiteral("messages"), QStringLiteral("file_type"))) {
        versionTenStatements.push_back(
            QStringLiteral("ALTER TABLE messages ADD COLUMN file_type TEXT NOT NULL DEFAULT ''"));
    }
    if (!ColumnExists(database_, QStringLiteral("messages"), QStringLiteral("file_size"))) {
        versionTenStatements.push_back(
            QStringLiteral("ALTER TABLE messages ADD COLUMN file_size INTEGER NOT NULL DEFAULT 0"));
    }
    if (!ColumnExists(database_, QStringLiteral("messages"), QStringLiteral("file_expired"))) {
        versionTenStatements.push_back(
            QStringLiteral("ALTER TABLE messages ADD COLUMN file_expired INTEGER NOT NULL DEFAULT 0"));
    }
    if (schemaVersion < 10 && !applyMigration(10, versionTenStatements)) {
        database_.rollback();
        return false;
    }

    if (!database_.commit()) {
        SetError(database_.lastError().text());
        return false;
    }
    return true;
}

bool SqliteClientRepository::SeedIfEmpty()
{
    QSqlQuery countQuery(database_);
    if (!countQuery.exec(QStringLiteral("SELECT COUNT(*) FROM conversations")) || !countQuery.next()) {
        SetError(countQuery.lastError().text());
        return false;
    }
    if (countQuery.value(0).toInt() > 0) {
        return true;
    }

    const FakeScenarioRepository fakeRepository;
    if (!database_.transaction()) {
        return false;
    }
    if (!InsertSnapshot(fakeRepository.LoadScenario(QStringLiteral("normal")))) {
        database_.rollback();
        return false;
    }
    if (!database_.commit()) {
        SetError(database_.lastError().text());
        return false;
    }
    return true;
}

bool SqliteClientRepository::InsertSnapshot(const ClientSnapshot& snapshot)
{
    int sortOrder = 0;
    for (const auto& conversation : snapshot.conversations) {
        QSqlQuery query(database_);
        query.prepare(QStringLiteral("INSERT INTO conversations(conversation_id, title, preview, timestamp, "
                                     "avatar_color, remote_conversation_id, unread_count, pinned, muted, "
                                     "online, sort_order) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        query.addBindValue(conversation.conversationId);
        query.addBindValue(conversation.title);
        query.addBindValue(conversation.preview);
        query.addBindValue(conversation.timestamp);
        query.addBindValue(conversation.avatarColor);
        query.addBindValue(conversation.remoteConversationId.has_value()
                               ? QVariant::fromValue<qlonglong>(*conversation.remoteConversationId)
                               : QVariant{});
        query.addBindValue(conversation.unreadCount);
        query.addBindValue(conversation.pinned);
        query.addBindValue(conversation.muted);
        query.addBindValue(conversation.online);
        query.addBindValue(sortOrder++);
        if (!query.exec()) {
            SetError(query.lastError().text());
            return false;
        }

        for (const auto& message : snapshot.messagesByConversation.value(conversation.conversationId)) {
            if (!InsertMessage(conversation.conversationId, message, false)) {
                return false;
            }
        }
    }

    for (const auto& contact : snapshot.contacts) {
        QSqlQuery query(database_);
        query.prepare(QStringLiteral("INSERT INTO contacts(user_id, display_name, "
                                     "status_text, avatar_color, "
                                     "online, favorite) VALUES(?, ?, ?, ?, ?, ?)"));
        query.addBindValue(contact.userId);
        query.addBindValue(contact.displayName);
        query.addBindValue(contact.statusText);
        query.addBindValue(contact.avatarColor);
        query.addBindValue(contact.online);
        query.addBindValue(contact.favorite);
        if (!query.exec()) {
            SetError(query.lastError().text());
            return false;
        }
    }

    int requestOrder = 0;
    for (const auto& request : snapshot.requests) {
        QSqlQuery query(database_);
        query.prepare(QStringLiteral("INSERT INTO requests(request_id, display_name, message, avatar_color, "
                                     "kind, status, local_order) VALUES(?, ?, ?, ?, ?, ?, ?)"));
        query.addBindValue(request.requestId);
        query.addBindValue(request.displayName);
        query.addBindValue(request.message);
        query.addBindValue(request.avatarColor);
        query.addBindValue(request.kind);
        query.addBindValue(request.status);
        query.addBindValue(requestOrder++);
        if (!query.exec()) {
            SetError(query.lastError().text());
            return false;
        }
    }
    return true;
}

bool SqliteClientRepository::InsertMessage(const QString& conversationId, const MessageRecord& message,
                                           bool ignoreDuplicate)
{
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT %1 INTO messages(client_message_id, message_id, "
                                 "conversation_id, "
                                 "conversation_seq, sender_id, body, type, file_id, file_name, file_type, "
                                 "file_size, file_expired, timestamp, outgoing, delivery_state) "
                                 "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
                      .arg(ignoreDuplicate ? QStringLiteral("OR IGNORE") : QString{}));
    query.addBindValue(QVariant::fromValue<qlonglong>(message.clientMessageId));
    query.addBindValue(message.messageId.has_value() ? QVariant::fromValue<qlonglong>(*message.messageId) : QVariant{});
    query.addBindValue(conversationId);
    query.addBindValue(message.conversationSeq.has_value() ? QVariant::fromValue<qlonglong>(*message.conversationSeq)
                                                           : QVariant{});
    query.addBindValue(message.senderId);
    query.addBindValue(message.body);
    query.addBindValue(message.type);
    query.addBindValue(message.fileId.isNull() ? QStringLiteral("") : message.fileId);
    query.addBindValue(message.fileName.isNull() ? QStringLiteral("") : message.fileName);
    query.addBindValue(message.fileType.isNull() ? QStringLiteral("") : message.fileType);
    query.addBindValue(QVariant::fromValue<qlonglong>(message.fileSize));
    query.addBindValue(message.fileExpired);
    query.addBindValue(message.timestamp);
    query.addBindValue(message.outgoing);
    query.addBindValue(DeliveryStateValue(message.deliveryState));
    if (!query.exec()) {
        SetError(query.lastError().text());
        return false;
    }
    return true;
}

void SqliteClientRepository::SetError(const QString& error) const { last_error_ = error; }

} // namespace wimi::client
