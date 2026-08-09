#pragma once

#include "adapters/connection_gateway/ConnectionGatewayClient.h"
#include "adapters/gate/GateHttpClient.h"
#include "models/ConversationListModel.h"
#include "models/ContactListModel.h"
#include "models/MessageListModel.h"
#include "models/RequestListModel.h"
#include "ports/IClientRepository.h"
#include "ports/IPlatformServices.h"

#include <QObject>
#include <QHash>
#include <QMap>
#include <QQmlEngine>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <cstdint>
#include <memory>

namespace wimi::client
{

class AppController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(ConversationListModel* conversations READ conversations CONSTANT)
    Q_PROPERTY(MessageListModel* messages READ messages CONSTANT)
    Q_PROPERTY(ContactListModel* contacts READ contacts CONSTANT)
    Q_PROPERTY(RequestListModel* requests READ requests CONSTANT)
    Q_PROPERTY(QStringList scenarios READ scenarios CONSTANT)
    Q_PROPERTY(QString scenarioName READ scenarioName WRITE setScenarioName NOTIFY scenarioNameChanged)
    Q_PROPERTY(QString connectionStatus READ connectionStatus NOTIFY connectionStatusChanged)
    Q_PROPERTY(int selectedConversationIndex READ selectedConversationIndex NOTIFY selectedConversationChanged)
    Q_PROPERTY(QString selectedConversationTitle READ selectedConversationTitle NOTIFY selectedConversationChanged)
    Q_PROPERTY(bool selectedConversationIsGroup READ selectedConversationIsGroup NOTIFY selectedConversationChanged)
    Q_PROPERTY(
        int selectedConversationUnreadCount READ selectedConversationUnreadCount NOTIFY selectedConversationChanged)
    Q_PROPERTY(QString draftText READ draftText WRITE setDraftText NOTIFY draftTextChanged)
    Q_PROPERTY(QString currentSection READ currentSection WRITE setCurrentSection NOTIFY currentSectionChanged)
    Q_PROPERTY(int selectedContactIndex READ selectedContactIndex NOTIFY selectedContactChanged)
    Q_PROPERTY(QString selectedContactName READ selectedContactName NOTIFY selectedContactChanged)
    Q_PROPERTY(QString selectedContactStatus READ selectedContactStatus NOTIFY selectedContactChanged)
    Q_PROPERTY(bool authRequired READ authRequired NOTIFY authRequiredChanged)
    Q_PROPERTY(bool networkEnabled READ networkEnabled CONSTANT)
    Q_PROPERTY(bool authenticationBusy READ authenticationBusy NOTIFY authenticationStateChanged)
    Q_PROPERTY(QString authenticationError READ authenticationError NOTIFY authenticationStateChanged)
    Q_PROPERTY(QString serviceActionStatus READ serviceActionStatus NOTIFY serviceActionStatusChanged)
    Q_PROPERTY(QString gateUrl READ gateUrl NOTIFY gateConfigurationChanged)
    Q_PROPERTY(QString gateConfigurationStatus READ gateConfigurationStatus NOTIFY gateConfigurationChanged)
    Q_PROPERTY(int requestedWindowWidth READ requestedWindowWidth CONSTANT)
    Q_PROPERTY(int requestedWindowHeight READ requestedWindowHeight CONSTANT)
    Q_PROPERTY(bool darkThemeRequested READ darkThemeRequested CONSTANT)
    Q_PROPERTY(bool compactConversationRequested READ compactConversationRequested CONSTANT)
    Q_PROPERTY(QString repositoryKind READ repositoryKind CONSTANT)
    Q_PROPERTY(QString platformName READ platformName CONSTANT)
    Q_PROPERTY(bool desktopNotificationsAvailable READ desktopNotificationsAvailable CONSTANT)
    Q_PROPERTY(QString notificationTestStatus READ notificationTestStatus NOTIFY notificationTestStatusChanged)
    Q_PROPERTY(qulonglong filePreviewRevision READ filePreviewRevision NOTIFY filePreviewRevisionChanged)

  public:
    explicit AppController(QObject* parent = nullptr);
    explicit AppController(std::unique_ptr<IPlatformServices> platformServices, QObject* parent = nullptr);
    ~AppController() override;

    ConversationListModel* conversations();
    MessageListModel* messages();
    ContactListModel* contacts();
    RequestListModel* requests();
    QStringList scenarios() const;
    QString scenarioName() const;
    QString connectionStatus() const;
    int selectedConversationIndex() const;
    QString selectedConversationTitle() const;
    bool selectedConversationIsGroup() const;
    int selectedConversationUnreadCount() const;
    QString draftText() const;
    QString currentSection() const;
    int selectedContactIndex() const;
    QString selectedContactName() const;
    QString selectedContactStatus() const;
    bool authRequired() const;
    bool networkEnabled() const;
    bool authenticationBusy() const;
    QString authenticationError() const;
    QString serviceActionStatus() const;
    QString gateUrl() const;
    QString gateConfigurationStatus() const;
    int requestedWindowWidth() const;
    int requestedWindowHeight() const;
    bool darkThemeRequested() const;
    bool compactConversationRequested() const;
    QString repositoryKind() const;
    QString platformName() const;
    bool desktopNotificationsAvailable() const;
    QString notificationTestStatus() const;
    qulonglong filePreviewRevision() const;

    void setScenarioName(const QString& scenarioName);
    void setDraftText(const QString& draftText);
    void setCurrentSection(const QString& currentSection);

    Q_INVOKABLE void selectConversation(int index);
    Q_INVOKABLE void sendMessage(const QString& text);
    Q_INVOKABLE void sendFile(const QUrl& fileUrl);
    Q_INVOKABLE void downloadFile(const QString& fileId, const QString& fileType, qint64 fileSize);
    Q_INVOKABLE QUrl localFileUrl(const QString& fileId) const;
    Q_INVOKABLE QUrl previewFileUrl(const QString& fileId) const;
    Q_INVOKABLE QString localMarkdownText(const QString& fileId) const;
    Q_INVOKABLE void retryMessage(qlonglong clientMessageId);
    Q_INVOKABLE void togglePinned(int index);
    Q_INVOKABLE void toggleMuted(int index);
    Q_INVOKABLE void markRead(int index);
    Q_INVOKABLE qreal conversationListPosition() const;
    Q_INVOKABLE void saveConversationListPosition(qreal position);
    Q_INVOKABLE qreal timelinePosition() const;
    Q_INVOKABLE void saveTimelinePosition(qreal position);
    Q_INVOKABLE void selectContact(int index);
    Q_INVOKABLE void toggleContactFavorite(int index);
    Q_INVOKABLE void resolveRequest(int index, bool accepted);
    Q_INVOKABLE void authenticate(const QString& username, const QString& password);
    Q_INVOKABLE void requestVerificationCode(const QString& email);
    Q_INVOKABLE void registerAccount(const QString& username, const QString& password, const QString& email,
                                     const QString& verificationCode);
    Q_INVOKABLE void resetPassword(const QString& username, const QString& email, const QString& verificationCode,
                                   const QString& newPassword);
    Q_INVOKABLE void startConversationWithSelectedContact();
    Q_INVOKABLE void sendFriendRequest(const QString& uid, const QString& message);
    Q_INVOKABLE void createGroup(const QString& name);
    Q_INVOKABLE void joinGroup(const QString& groupId, const QString& message);
    Q_INVOKABLE void completeAuthentication();
    Q_INVOKABLE void saveGateUrl(const QString& gateUrl);
    Q_INVOKABLE void sendTestDesktopNotification();

  signals:
    void scenarioNameChanged();
    void connectionStatusChanged();
    void selectedConversationChanged();
    void draftTextChanged();
    void currentSectionChanged();
    void selectedContactChanged();
    void authRequiredChanged();
    void authenticationStateChanged();
    void authenticationOperationSucceeded(const QString& operation, const QString& message);
    void serviceActionStatusChanged();
    void gateConfigurationChanged();
    void notificationTestStatusChanged();
    void filePreviewRevisionChanged();

  private:
    void LoadScenario(const QString& scenarioName);
    void ScheduleDeliveryLifecycle(std::int64_t clientMessageId);
    void UpdateMessageState(std::int64_t clientMessageId, MessageDeliveryState state);
    void ConfigureNetwork(const QString& gateUrl);
    void HandleGatewayResponse(const QString& requestId, quint32 serviceId, const QByteArray& payload);
    void HandleGatewayPush(quint32 serviceId, const QByteArray& payload);
    void HandleGatewayFailure(const QString& requestId, quint32 serviceId, int errorCode, const QString& message,
                              bool outcomeUnknown);
    void SyncKnownConversations();
    void ScheduleGapSync(const QString& localConversationId);
    void DrainPendingPush(const QString& localConversationId);
    void ResumePendingOutgoing();
    void SendNextFileChunk(const QString& transferId);
    void SendCompletedFileMessage(const QString& transferId);
    void StartAutoDownload(const wimi::protocol::FileInfo& file);
    void StartDownload(const wimi::protocol::FileInfo& file);
    void RequestNextDownloadChunk(const QString& fileId);
    void AcknowledgeConversationRead(int index);
    void SetRequestStatus(int index, const QString& status, bool persist);
    QString EnsureDirectConversation(std::int64_t peerUid, const QString& title, const QString& avatarColor);
    QString EnsureGroupConversation(std::int64_t groupId, const QString& title);
    void AttachRemoteConversation(const QString& localConversationId, std::int64_t remoteConversationId);
    void SetConnectionStatus(const QString& status);
    void SetServiceActionStatus(const QString& status);
    QString CurrentConversationId() const;
    QString CurrentStateKey() const;
    static QString ArgumentValue(const QString& name, const QString& fallback);

    std::unique_ptr<IClientRepository> repository_;
    std::unique_ptr<IPlatformServices> platform_services_;
    GateHttpClient gate_client_;
    ConnectionGatewayClient gateway_client_;
    ConversationListModel conversations_;
    MessageListModel messages_;
    ContactListModel contacts_;
    RequestListModel requests_;
    ClientSnapshot snapshot_;
    int selected_conversation_index_{-1};
    std::int64_t next_client_message_id_{-1000};
    std::int64_t next_fake_group_id_{900000};
    int requested_window_width_{1280};
    int requested_window_height_{800};
    bool dark_theme_requested_{};
    bool compact_conversation_requested_{};
    QHash<QString, QString> drafts_;
    QHash<QString, qreal> timeline_positions_;
    qreal conversation_list_position_{};
    QString current_section_{QStringLiteral("chats")};
    int selected_contact_index_{-1};
    bool authentication_completed_{};
    bool network_enabled_{};
    bool authentication_busy_{};
    std::int64_t authenticated_uid_{};
    QHash<QString, std::int64_t> outgoing_request_messages_;
    struct FileUpload {
        QString sourcePath;
        QString fileId;
        QString fileName;
        QString fileType;
        QByteArray sha256;
        qint64 fileSize{};
        qint64 nextOffset{};
        QString conversationId;
        bool uploadComplete{};
    };
    QHash<QString, FileUpload> file_uploads_;
    QHash<QString, QString> local_file_sources_;
    QHash<QString, QString> file_upload_requests_;
    QHash<QString, QString> file_message_requests_;
    struct FileDownload {
        QString fileId;
        QString fileType;
        qint64 fileSize{};
        qint64 nextOffset{};
        QByteArray sha256;
        QString temporaryPath;
        QString finalPath;
    };
    QHash<QString, FileDownload> file_downloads_;
    QHash<QString, QString> file_download_requests_;
    QHash<QString, QString> sync_request_conversations_;
    QHash<QString, QMap<std::int64_t, QPair<quint32, QByteArray>>> pending_pushes_;
    QHash<QString, QTimer*> gap_timers_;
    QSet<QString> gap_syncing_;
    QHash<QString, int> friend_reply_requests_;
    QHash<QString, QString> create_group_requests_;
    QString authentication_error_;
    QString service_action_status_;
    QString gate_url_;
    QString gate_configuration_status_{QStringLiteral("idle")};
    QString notification_test_status_{QStringLiteral("idle")};
    qulonglong file_preview_revision_{};
};

} // namespace wimi::client
