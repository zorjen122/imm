#pragma once

#include "adapters/connection_gateway/TcpFrameCodec.h"
#include "adapters/gate/GateHttpClient.h"

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QTcpSocket>
#include <QTimer>

#include <cstdint>

namespace wimi::protocol
{
class Packet;
class FileInfo;
} // namespace wimi::protocol

namespace wimi::client
{

class ConnectionGatewayClient final : public QObject
{
    Q_OBJECT

  public:
    enum class State {
        Disconnected,
        Connecting,
        Authenticating,
        Ready,
        Reconnecting,
        Closing,
    };
    Q_ENUM(State)

    explicit ConnectionGatewayClient(QObject* parent = nullptr);
    ~ConnectionGatewayClient() override;

    State CurrentState() const;
    bool IsReady() const;
    std::int64_t UserId() const;
    QString StableClientMessageId(std::int64_t localMessageId) const;

    void Open(const GateSession& session);
    void Close();

    QString PullFriendList();
    QString PullFriendApplications();
    QString PullConversationMessages(std::int64_t conversationId, std::int64_t afterSequence, int limit = 50);
    QString SendFriendRequest(std::int64_t recipientUid, const QString& message);
    QString ReplyFriendRequest(std::int64_t recipientUid, bool accept, const QString& message);
    QString SendText(std::int64_t recipientUid, const QByteArray& utf8Text, const QString& clientMessageId,
                     std::int64_t conversationId = 0);
    QString UploadFile(std::int64_t clientSequence, const QString& fileName, const QString& fileType,
                       const QByteArray& content);
    QString UploadFileChunk(const wimi::protocol::FileInfo& file, std::int64_t offset, const QByteArray& content,
                            bool complete);
    QString SendFile(std::int64_t recipientUid, const wimi::protocol::FileInfo& file, const QString& clientMessageId,
                     std::int64_t conversationId = 0);
    QString DownloadFileChunk(const wimi::protocol::FileInfo& file, std::int64_t offset);
    QString CreateGroup(const QString& groupName);
    QString RequestJoinGroup(std::int64_t groupId, const QString& message);
    QString ReplyJoinGroup(std::int64_t groupId, std::int64_t requestorUid, bool accept);
    QString PullGroupMembers(std::int64_t groupId);
    QString QuitGroup(std::int64_t groupId);
    QString KickGroupMember(std::int64_t groupId, std::int64_t targetUid);
    QString SendGroupText(std::int64_t groupId, const QByteArray& utf8Text, const QString& clientMessageId,
                          std::int64_t conversationId = 0);
    QString SendGroupFile(std::int64_t groupId, const wimi::protocol::FileInfo& file, const QString& clientMessageId,
                          std::int64_t conversationId = 0);

    void AcknowledgeDelivered(std::int64_t messageId, std::int64_t conversationId, std::int64_t conversationSequence);
    void AcknowledgeRead(std::int64_t messageId, std::int64_t conversationId, std::int64_t conversationSequence);

  signals:
    void StateChanged(wimi::client::ConnectionGatewayClient::State state);
    void Authenticated();
    void CredentialsExpired();
    void ResponseReceived(const QString& requestId, quint32 serviceId, const QByteArray& payload);
    void PushReceived(quint32 serviceId, const QByteArray& payload);
    void RequestFailed(const QString& requestId, quint32 requestServiceId, int errorCode, const QString& message,
                       bool outcomeUnknown);
    void ProtocolError(const QString& message);

  private:
    struct PendingRequest {
        QString requestId;
        quint32 requestServiceId{};
        quint32 responseServiceId{};
        QByteArray payload;
        int timeoutMilliseconds{};
    };

    QString QueueRequest(quint32 serviceId, wimi::protocol::Packet packet, int timeoutMilliseconds = 12'000);
    void SendFront(quint32 responseServiceId);
    void CompleteFront(quint32 responseServiceId, const QByteArray& payload);
    void FailAllPending(int errorCode, const QString& message, bool outcomeUnknown);
    void SendReceipt(std::int64_t messageId, int receiptType, std::int64_t conversationId,
                     std::int64_t conversationSequence);
    void SendPacket(quint32 serviceId, wimi::protocol::Packet packet);
    void HandleFrame(const TcpFrame& frame);
    void HandleLoginResponse(const QByteArray& payload);
    void ScheduleReconnect();
    void StartSocket();
    void SetState(State state);

    QTcpSocket socket_;
    TcpFrameCodec codec_;
    QTimer heartbeat_timer_;
    QTimer reconnect_timer_;
    QHash<quint32, QQueue<PendingRequest>> pending_by_response_;
    QHash<quint32, QTimer*> request_timers_;
    GateSession session_;
    QString device_id_;
    State state_{State::Disconnected};
    qint64 token_expires_at_milliseconds_{};
    int reconnect_attempt_{};
    bool desired_open_{};
};

} // namespace wimi::client

Q_DECLARE_METATYPE(wimi::client::ConnectionGatewayClient::State)
