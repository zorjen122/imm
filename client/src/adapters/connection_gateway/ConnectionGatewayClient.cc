#include "adapters/connection_gateway/ConnectionGatewayClient.h"

#include "adapters/connection_gateway/ClientProtocol.h"
#include "adapters/connection_gateway/ProtobufPacketCodec.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QSettings>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace wimi::client
{
namespace
{

constexpr int kHeartbeatIntervalMilliseconds = 20'000;
constexpr int kReconnectBaseMilliseconds = 200;
constexpr int kReconnectMaximumMilliseconds = 10'000;

QString NewRequestId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

} // namespace

ConnectionGatewayClient::ConnectionGatewayClient(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<State>();
    heartbeat_timer_.setInterval(kHeartbeatIntervalMilliseconds);
    reconnect_timer_.setSingleShot(true);

    connect(&socket_, &QTcpSocket::connected, this, [this] {
        reconnect_attempt_ = 0;
        SetState(State::Authenticating);
        wimi::protocol::LoginRequest login;
        login.setUid(session_.uid);
        login.setDeviceId(device_id_);
        login.setAuthToken(session_.token);
        login.setInitProfile(session_.profileInitializationRequired);
        if (session_.profileInitializationRequired) {
            login.setName(session_.profileName);
            login.setAge(0);
            login.setSex(QString{});
            login.setHeadImageUrl(QString{});
        }
        wimi::protocol::Packet packet;
        packet.setLoginRequest(std::move(login));
        SendPacket(protocol::Operation::OP_LOGIN, std::move(packet));
    });
    connect(&socket_, &QTcpSocket::readyRead, this, [this] {
        const auto frames = codec_.Feed(socket_.readAll());
        if (codec_.HasError()) {
            emit ProtocolError(codec_.ErrorString());
            socket_.abort();
            return;
        }
        for (const auto& frame : frames) {
            HandleFrame(frame);
        }
    });
    connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        heartbeat_timer_.stop();
        codec_.Reset();
        FailAllPending(protocol::DependencyUnavailable, tr("Connection Gateway 连接已断开"), true);
        if (desired_open_ && state_ != State::Closing) {
            ScheduleReconnect();
        } else {
            SetState(State::Disconnected);
        }
    });
    connect(&socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (state_ == State::Connecting || state_ == State::Reconnecting) {
            emit ProtocolError(socket_.errorString());
        }
    });
    connect(&heartbeat_timer_, &QTimer::timeout, this, [this] {
        if (!IsReady()) {
            return;
        }
        wimi::protocol::Packet ping;
        ping.setPingRequest(wimi::protocol::PingRequest{});
        SendPacket(protocol::Operation::OP_PING, std::move(ping));
    });
    connect(&reconnect_timer_, &QTimer::timeout, this, &ConnectionGatewayClient::StartSocket);
}

ConnectionGatewayClient::~ConnectionGatewayClient()
{
    desired_open_ = false;
    QObject::disconnect(&socket_, nullptr, this, nullptr);
    heartbeat_timer_.stop();
    reconnect_timer_.stop();
    for (auto* timer : std::as_const(request_timers_)) {
        timer->stop();
        timer->deleteLater();
    }
    request_timers_.clear();
    socket_.abort();
}

ConnectionGatewayClient::State ConnectionGatewayClient::CurrentState() const { return state_; }

bool ConnectionGatewayClient::IsReady() const { return state_ == State::Ready; }

std::int64_t ConnectionGatewayClient::UserId() const { return session_.uid; }

QString ConnectionGatewayClient::StableClientMessageId(std::int64_t localMessageId) const
{
    return QStringLiteral("%1:%2").arg(device_id_).arg(-localMessageId);
}

void ConnectionGatewayClient::Open(const GateSession& session)
{
    desired_open_ = false;
    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        socket_.abort();
    }
    session_ = session;
    QSettings settings;
    const QString deviceKey = QStringLiteral("devices/%1/id").arg(session.uid);
    device_id_ = settings.value(deviceKey).toString();
    if (device_id_.isEmpty()) {
        device_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(deviceKey, device_id_);
        settings.sync();
    }
    token_expires_at_milliseconds_ =
        QDateTime::currentMSecsSinceEpoch() + std::max<qint64>(0, session.tokenExpiresInSeconds) * 1000;
    reconnect_attempt_ = 0;
    reconnect_timer_.stop();
    heartbeat_timer_.stop();
    desired_open_ = true;
    StartSocket();
}

void ConnectionGatewayClient::Close()
{
    desired_open_ = false;
    reconnect_timer_.stop();
    heartbeat_timer_.stop();
    if (socket_.state() == QAbstractSocket::UnconnectedState) {
        SetState(State::Disconnected);
        return;
    }
    SetState(State::Closing);
    wimi::protocol::Packet quit;
    quit.setQuitRequest(wimi::protocol::QuitRequest{});
    SendPacket(protocol::Operation::OP_QUIT, std::move(quit));
    QTimer::singleShot(1000, this, [this] {
        if (state_ == State::Closing) {
            socket_.disconnectFromHost();
        }
    });
}

QString ConnectionGatewayClient::PullFriendList()
{
    wimi::protocol::Packet packet;
    packet.setPullFriendListRequest(wimi::protocol::PullFriendListRequest{});
    return QueueRequest(protocol::Operation::OP_PULL_FRIEND_LIST, std::move(packet));
}

QString ConnectionGatewayClient::PullFriendApplications()
{
    wimi::protocol::Packet packet;
    packet.setPullFriendApplyListRequest(wimi::protocol::PullFriendApplyListRequest{});
    return QueueRequest(protocol::Operation::OP_PULL_FRIEND_APPLICATIONS, std::move(packet));
}

QString ConnectionGatewayClient::PullConversationMessages(std::int64_t conversationId, std::int64_t afterSequence,
                                                          int limit)
{
    wimi::protocol::PullConversationMessagesRequest request;
    request.setConversationId(conversationId);
    request.setAfterSeq(std::max<std::int64_t>(0, afterSequence));
    request.setLimit(std::clamp(limit, 1, 200));
    wimi::protocol::Packet packet;
    packet.setPullConversationMessagesRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_PULL_CONVERSATION_MESSAGES, std::move(packet));
}

QString ConnectionGatewayClient::SendFriendRequest(std::int64_t recipientUid, const QString& message)
{
    wimi::protocol::AddFriendRequest request;
    request.setTo(recipientUid);
    request.setRequestMessage(message);
    wimi::protocol::Packet packet;
    packet.setAddFriendRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_ADD_FRIEND, std::move(packet));
}

QString ConnectionGatewayClient::ReplyFriendRequest(std::int64_t recipientUid, bool accept, const QString& message)
{
    wimi::protocol::ReplyFriendRequest request;
    request.setTo(recipientUid);
    request.setAccept(accept);
    request.setReplyMessage(message);
    wimi::protocol::Packet packet;
    packet.setReplyFriendRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_REPLY_FRIEND, std::move(packet));
}

QString ConnectionGatewayClient::SendText(std::int64_t recipientUid, const QByteArray& utf8Text,
                                          const QString& clientMessageId, std::int64_t conversationId)
{
    wimi::protocol::SendTextRequest request;
    request.setTo(recipientUid);
    request.setContent(utf8Text);
    request.setClientMessageId(clientMessageId);
    if (conversationId > 0) {
        request.setConversationId(conversationId);
    }
    wimi::protocol::Packet packet;
    packet.setSendTextRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_SEND_TEXT, std::move(packet));
}

QString ConnectionGatewayClient::UploadFile(std::int64_t clientSequence, const QString& fileName,
                                            const QString& fileType, const QByteArray& content)
{
    (void)clientSequence;
    wimi::protocol::FileInfo file;
    file.setFileName(fileName);
    file.setFileType(fileType);
    file.setFileSize(content.size());
    wimi::protocol::FileUploadRequest request;
    request.setFile(std::move(file));
    request.setData(content);
    request.setComplete(true);
    wimi::protocol::Packet packet;
    packet.setFileUploadRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_UPLOAD_FILE, std::move(packet), 15'000);
}

QString ConnectionGatewayClient::UploadFileChunk(const wimi::protocol::FileInfo& file, std::int64_t offset,
                                                 const QByteArray& content, bool complete)
{
    const auto operation = complete ? protocol::Operation::OP_COMPLETE_FILE_UPLOAD
                                    : (content.isEmpty() ? protocol::Operation::OP_UPLOAD_FILE
                                                         : protocol::Operation::OP_UPLOAD_FILE_CHUNK);
    wimi::protocol::Packet packet;
    if (operation == protocol::Operation::OP_COMPLETE_FILE_UPLOAD) {
        wimi::protocol::FileUploadCompleteRequest request;
        request.setFile(file);
        request.setOffset(std::max<std::int64_t>(0, offset));
        request.setData(content);
        request.setComplete(true);
        packet.setFileUploadCompleteRequest(std::move(request));
    } else if (operation == protocol::Operation::OP_UPLOAD_FILE_CHUNK) {
        wimi::protocol::FileUploadChunkRequest request;
        request.setFile(file);
        request.setOffset(std::max<std::int64_t>(0, offset));
        request.setData(content);
        request.setComplete(false);
        packet.setFileUploadChunkRequest(std::move(request));
    } else {
        wimi::protocol::FileUploadRequest request;
        request.setFile(file);
        request.setOffset(std::max<std::int64_t>(0, offset));
        request.setData(content);
        request.setComplete(false);
        packet.setFileUploadRequest(std::move(request));
    }
    return QueueRequest(operation, std::move(packet), 15'000);
}

QString ConnectionGatewayClient::SendFile(std::int64_t recipientUid, const wimi::protocol::FileInfo& file,
                                          const QString& clientMessageId, std::int64_t conversationId)
{
    wimi::protocol::SendFileRequest request;
    request.setTo(recipientUid);
    request.setFile(file);
    request.setClientMessageId(clientMessageId);
    if (conversationId > 0)
        request.setConversationId(conversationId);
    wimi::protocol::Packet packet;
    packet.setSendFileRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_SEND_FILE, std::move(packet));
}

QString ConnectionGatewayClient::DownloadFileChunk(const wimi::protocol::FileInfo& file, std::int64_t offset)
{
    wimi::protocol::FileDownloadRequest request;
    request.setFile(file);
    request.setOffset(std::max<std::int64_t>(0, offset));
    request.setLimit(1024 * 1024);
    wimi::protocol::Packet packet;
    packet.setFileDownloadRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_DOWNLOAD_FILE, std::move(packet), 15'000);
}

QString ConnectionGatewayClient::CreateGroup(const QString& groupName)
{
    wimi::protocol::CreateGroupRequest request;
    request.setGroupName(groupName);
    wimi::protocol::Packet packet;
    packet.setCreateGroupRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_CREATE_GROUP, std::move(packet));
}

QString ConnectionGatewayClient::RequestJoinGroup(std::int64_t groupId, const QString& message)
{
    wimi::protocol::JoinGroupRequest request;
    request.setGroupId(groupId);
    request.setRequestMessage(message);
    wimi::protocol::Packet packet;
    packet.setJoinGroupRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_JOIN_GROUP, std::move(packet));
}

QString ConnectionGatewayClient::ReplyJoinGroup(std::int64_t groupId, std::int64_t requestorUid, bool accept)
{
    wimi::protocol::ReplyJoinGroupRequest request;
    request.setGroupId(groupId);
    request.setRequestorUid(requestorUid);
    request.setAccept(accept);
    wimi::protocol::Packet packet;
    packet.setReplyJoinGroupRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_REPLY_JOIN_GROUP, std::move(packet));
}

QString ConnectionGatewayClient::PullGroupMembers(std::int64_t groupId)
{
    wimi::protocol::PullGroupMembersRequest request;
    request.setGroupId(groupId);
    wimi::protocol::Packet packet;
    packet.setPullGroupMembersRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_PULL_GROUP_MEMBERS, std::move(packet));
}

QString ConnectionGatewayClient::QuitGroup(std::int64_t groupId)
{
    wimi::protocol::QuitGroupRequest request;
    request.setGroupId(groupId);
    wimi::protocol::Packet packet;
    packet.setQuitGroupRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_QUIT_GROUP, std::move(packet));
}

QString ConnectionGatewayClient::KickGroupMember(std::int64_t groupId, std::int64_t targetUid)
{
    wimi::protocol::KickGroupMemberRequest request;
    request.setGroupId(groupId);
    request.setMemberUid(targetUid);
    wimi::protocol::Packet packet;
    packet.setKickGroupMemberRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_KICK_GROUP_MEMBER, std::move(packet));
}

QString ConnectionGatewayClient::SendGroupText(std::int64_t groupId, const QByteArray& utf8Text,
                                               const QString& clientMessageId, std::int64_t conversationId)
{
    wimi::protocol::SendGroupTextRequest request;
    request.setGroupId(groupId);
    request.setContent(utf8Text);
    request.setClientMessageId(clientMessageId);
    if (conversationId > 0) {
        request.setConversationId(conversationId);
    }
    wimi::protocol::Packet packet;
    packet.setSendGroupTextRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_SEND_GROUP_TEXT, std::move(packet));
}

QString ConnectionGatewayClient::SendGroupFile(std::int64_t groupId, const wimi::protocol::FileInfo& file,
                                               const QString& clientMessageId, std::int64_t conversationId)
{
    wimi::protocol::SendGroupFileRequest request;
    request.setGroupId(groupId);
    request.setFile(file);
    request.setClientMessageId(clientMessageId);
    if (conversationId > 0)
        request.setConversationId(conversationId);
    wimi::protocol::Packet packet;
    packet.setSendGroupFileRequest(std::move(request));
    return QueueRequest(protocol::Operation::OP_SEND_GROUP_FILE, std::move(packet));
}

void ConnectionGatewayClient::AcknowledgeDelivered(std::int64_t messageId, std::int64_t conversationId,
                                                   std::int64_t conversationSequence)
{
    SendReceipt(messageId, static_cast<int>(wimi::protocol::ReceiptTypeGadget::ReceiptType::RECEIPT_TYPE_DELIVERED),
                conversationId, conversationSequence);
}

void ConnectionGatewayClient::AcknowledgeRead(std::int64_t messageId, std::int64_t conversationId,
                                              std::int64_t conversationSequence)
{
    SendReceipt(messageId, static_cast<int>(wimi::protocol::ReceiptTypeGadget::ReceiptType::RECEIPT_TYPE_READ),
                conversationId, conversationSequence);
}

QString ConnectionGatewayClient::QueueRequest(protocol::Operation operation, wimi::protocol::Packet packet,
                                              int timeoutMilliseconds)
{
    const QString requestId = NewRequestId();
    if (!IsReady()) {
        emit RequestFailed(requestId, operation, protocol::DependencyUnavailable, tr("Connection Gateway 尚未就绪"),
                           false);
        return requestId;
    }

    wimi::protocol::ClientEnvelope envelope;
    wimi::protocol::ClientRequest request;
    request.setRequestId(requestId);
    request.setPacket(std::move(packet));
    request.setOperation(operation);
    envelope.setRequest(std::move(request));
    QByteArray payload;
    if (!SerializeClientEnvelope(envelope, &payload)) {
        emit RequestFailed(requestId, operation, protocol::JsonParser, tr("无法序列化请求"), false);
        return requestId;
    }

    pending_by_id_.insert(requestId, PendingRequest{
                                         .requestId = requestId,
                                         .operation = operation,
                                         .payload = payload,
                                         .timeoutMilliseconds = timeoutMilliseconds,
                                     });
    socket_.write(TcpFrameCodec::Encode(payload));

    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    request_timers_.insert(requestId, timer);
    connect(timer, &QTimer::timeout, this, [this, requestId] {
        const auto found = pending_by_id_.find(requestId);
        if (found == pending_by_id_.end()) {
            return;
        }
        const PendingRequest pending = found.value();
        pending_by_id_.erase(found);
        if (auto* timer = request_timers_.take(requestId)) {
            timer->deleteLater();
        }
        emit RequestFailed(pending.requestId, pending.operation, protocol::DeadlineExceeded, tr("请求等待响应超时"),
                           true);
    });
    timer->start(timeoutMilliseconds);
    return requestId;
}

void ConnectionGatewayClient::CompleteRequest(const QString& requestId, const QByteArray& payload)
{
    const auto found = pending_by_id_.find(requestId);
    if (found == pending_by_id_.end()) {
        emit ProtocolError(tr("收到没有在途请求的响应 request=%1").arg(requestId));
        return;
    }
    const PendingRequest completed = found.value();
    pending_by_id_.erase(found);
    if (auto* timer = request_timers_.take(requestId)) {
        timer->stop();
        timer->deleteLater();
    }
    emit ResponseReceived(completed.requestId, completed.operation, payload);
}

void ConnectionGatewayClient::FailAllPending(int errorCode, const QString& message, bool outcomeUnknown)
{
    for (auto* timer : std::as_const(request_timers_)) {
        timer->stop();
    }
    for (const auto& pending : std::as_const(pending_by_id_)) {
        emit RequestFailed(pending.requestId, pending.operation, errorCode, message, outcomeUnknown);
    }
    pending_by_id_.clear();
}

void ConnectionGatewayClient::SendReceipt(std::int64_t messageId, int receiptType, std::int64_t conversationId,
                                          std::int64_t conversationSequence)
{
    if (!IsReady() || messageId <= 0) {
        return;
    }
    wimi::protocol::MessageReceipt receipt;
    receipt.setSeq(messageId);
    receipt.setReceiptType(static_cast<wimi::protocol::ReceiptTypeGadget::ReceiptType>(receiptType));
    if (conversationId > 0 && conversationSequence > 0) {
        receipt.setConversationId(conversationId);
        receipt.setConversationSeq(conversationSequence);
    }
    wimi::protocol::Packet packet;
    packet.setReceipt(std::move(receipt));
    SendPacket(protocol::Operation::OP_ACKNOWLEDGE, std::move(packet));
}

void ConnectionGatewayClient::SendPacket(protocol::Operation operation, wimi::protocol::Packet packet)
{
    wimi::protocol::ClientEnvelope envelope;
    wimi::protocol::ClientRequest request;
    request.setRequestId(NewRequestId());
    request.setPacket(std::move(packet));
    request.setOperation(operation);
    envelope.setRequest(std::move(request));
    QByteArray payload;
    if (!SerializeClientEnvelope(envelope, &payload)) {
        emit ProtocolError(tr("无法序列化 operation=%1").arg(static_cast<int>(operation)));
        return;
    }
    socket_.write(TcpFrameCodec::Encode(payload));
}

void ConnectionGatewayClient::HandleFrame(const TcpFrame& frame)
{
    wimi::protocol::ClientEnvelope envelope;
    if (!ParseClientEnvelope(frame.payload, &envelope)) {
        emit ProtocolError(tr("无法解析 Gateway 信封"));
        socket_.abort();
        return;
    }
    if (envelope.hasResponse()) {
        const auto& response = envelope.response();
        const auto& packet = response.packet();
        if (packet.hasLoginResponse()) {
            HandleLoginResponse(packet);
            return;
        }
        if (packet.hasPingResponse()) {
            return;
        }
        if (packet.hasQuitResponse()) {
            desired_open_ = false;
            socket_.disconnectFromHost();
            return;
        }
        QByteArray payload;
        if (!SerializeProtobufPacket(packet, &payload)) {
            emit ProtocolError(tr("无法序列化 Gateway 响应"));
            return;
        }
        const auto pending = pending_by_id_.find(response.requestId());
        if (pending != pending_by_id_.end() && response.operation() != pending->operation) {
            emit ProtocolError(tr("Gateway 响应操作类型不匹配"));
            socket_.abort();
            return;
        }
        CompleteRequest(response.requestId(), payload);
        return;
    }
    if (envelope.hasPush()) {
        if (envelope.push().type() != protocol::PushTypeFor(envelope.push().packet())) {
            emit ProtocolError(tr("Gateway 推送类型不匹配"));
            socket_.abort();
            return;
        }
        QByteArray payload;
        if (!SerializeProtobufPacket(envelope.push().packet(), &payload)) {
            emit ProtocolError(tr("无法序列化 Gateway 推送"));
            return;
        }
        emit PushReceived(payload);
        return;
    }
    emit ProtocolError(tr("Gateway 返回了客户端不应接收的请求信封"));
}

void ConnectionGatewayClient::HandleLoginResponse(const wimi::protocol::Packet& response)
{
    const int errorCode =
        response.hasStatus() && response.status().hasError() ? static_cast<int>(response.status().error()) : 0;
    if (errorCode != protocol::Success) {
        desired_open_ = false;
        if (errorCode == protocol::TokenInvalid || errorCode == protocol::AuthenticationRequired) {
            emit CredentialsExpired();
        } else {
            emit ProtocolError(tr("Gateway 登录失败（%1）").arg(errorCode));
        }
        socket_.disconnectFromHost();
        return;
    }

    session_.profileInitializationRequired = false;
    SetState(State::Ready);
    heartbeat_timer_.start();
    emit Authenticated();
}

void ConnectionGatewayClient::ScheduleReconnect()
{
    if (token_expires_at_milliseconds_ > 0 && QDateTime::currentMSecsSinceEpoch() >= token_expires_at_milliseconds_) {
        desired_open_ = false;
        SetState(State::Disconnected);
        emit CredentialsExpired();
        return;
    }

    SetState(State::Reconnecting);
    const int exponent = std::min(reconnect_attempt_, 6);
    const int base = std::min(kReconnectMaximumMilliseconds, kReconnectBaseMilliseconds * (1 << exponent));
    const int jitter = QRandomGenerator::global()->bounded(base / 4 + 1);
    ++reconnect_attempt_;
    reconnect_timer_.start(std::min(kReconnectMaximumMilliseconds, base + jitter));
}

void ConnectionGatewayClient::StartSocket()
{
    if (!desired_open_) {
        return;
    }
    codec_.Reset();
    SetState(reconnect_attempt_ == 0 ? State::Connecting : State::Reconnecting);
    socket_.connectToHost(session_.gatewayHost, session_.gatewayPort);
}

void ConnectionGatewayClient::SetState(State state)
{
    if (state_ == state) {
        return;
    }
    state_ = state;
    emit StateChanged(state_);
}

} // namespace wimi::client
