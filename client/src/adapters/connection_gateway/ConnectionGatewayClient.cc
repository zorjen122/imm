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
constexpr int kServerRequestBudgetMilliseconds = 10'000;

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
        wimi::protocol::Packet login;
        login.setUid(session_.uid);
        login.setDeviceId(device_id_);
        login.setAuthToken(session_.token);
        login.setInit(session_.profileInitializationRequired);
        if (session_.profileInitializationRequired) {
            login.setName(session_.profileName);
            login.setAge(0);
            login.setSex(QString{});
            login.setHeadImageUrl(QString{});
        }
        SendPacket(protocol::LoginRequest, login);
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
        SendPacket(protocol::PingRequest, ping);
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
    }
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
    SendPacket(protocol::QuitRequest, quit);
    QTimer::singleShot(1000, this, [this] {
        if (state_ == State::Closing) {
            socket_.disconnectFromHost();
        }
    });
}

QString ConnectionGatewayClient::PullFriendList() { return QueueRequest(protocol::PullFriendListRequest, {}); }

QString ConnectionGatewayClient::PullFriendApplications()
{
    return QueueRequest(protocol::PullFriendApplyListRequest, {});
}

QString ConnectionGatewayClient::PullConversationMessages(std::int64_t conversationId, std::int64_t afterSequence,
                                                          int limit)
{
    wimi::protocol::Packet packet;
    packet.setConversationId(conversationId);
    packet.setAfterSeq(std::max<std::int64_t>(0, afterSequence));
    packet.setLimit(std::clamp(limit, 1, 200));
    return QueueRequest(protocol::PullSessionMessagesRequest, std::move(packet));
}

QString ConnectionGatewayClient::SendFriendRequest(std::int64_t recipientUid, const QString& message)
{
    wimi::protocol::Packet packet;
    packet.setTo(recipientUid);
    packet.setRequestMessage(message);
    return QueueRequest(protocol::AddFriendRequest, std::move(packet));
}

QString ConnectionGatewayClient::ReplyFriendRequest(std::int64_t recipientUid, bool accept, const QString& message)
{
    wimi::protocol::Packet packet;
    packet.setTo(recipientUid);
    packet.setAccept(accept);
    packet.setReplyMessage(message);
    return QueueRequest(protocol::ReplyFriendRequest, std::move(packet));
}

QString ConnectionGatewayClient::SendText(std::int64_t recipientUid, const QByteArray& utf8Text,
                                          const QString& clientMessageId, std::int64_t conversationId)
{
    wimi::protocol::Packet packet;
    packet.setTo(recipientUid);
    packet.setData(utf8Text);
    packet.setClientMessageId(clientMessageId);
    if (conversationId > 0) {
        packet.setConversationId(conversationId);
    }
    return QueueRequest(protocol::SendTextRequest, std::move(packet));
}

QString ConnectionGatewayClient::UploadFile(std::int64_t clientSequence, const QString& fileName,
                                            const QString& fileType, const QByteArray& content)
{
    wimi::protocol::Packet packet;
    packet.setSeq(clientSequence);
    packet.setFileName(fileName);
    packet.setFileType(fileType);
    packet.setData(content);
    return QueueRequest(protocol::UploadFileRequest, std::move(packet), 15'000);
}

QString ConnectionGatewayClient::UploadFileChunk(const wimi::protocol::FileInfo& file, std::int64_t offset,
                                                 const QByteArray& content, bool complete)
{
    wimi::protocol::Packet packet;
    packet.setFile(file);
    packet.setFileOffset(std::max<std::int64_t>(0, offset));
    packet.setData(content);
    packet.setFileComplete(complete);
    const auto service = complete
                             ? protocol::FileUploadCompleteRequest
                             : (content.isEmpty() ? protocol::UploadFileRequest : protocol::FileUploadChunkRequest);
    return QueueRequest(service, std::move(packet), 15'000);
}

QString ConnectionGatewayClient::SendFile(std::int64_t recipientUid, const wimi::protocol::FileInfo& file,
                                          const QString& clientMessageId, std::int64_t conversationId)
{
    wimi::protocol::Packet packet;
    packet.setTo(recipientUid);
    packet.setFile(file);
    packet.setClientMessageId(clientMessageId);
    if (conversationId > 0)
        packet.setConversationId(conversationId);
    return QueueRequest(protocol::SendFileRequest, std::move(packet));
}

QString ConnectionGatewayClient::DownloadFileChunk(const wimi::protocol::FileInfo& file, std::int64_t offset)
{
    wimi::protocol::Packet packet;
    packet.setFile(file);
    packet.setFileOffset(std::max<std::int64_t>(0, offset));
    packet.setLimit(1024 * 1024);
    return QueueRequest(protocol::FileDownloadRequest, std::move(packet), 15'000);
}

QString ConnectionGatewayClient::CreateGroup(const QString& groupName)
{
    wimi::protocol::Packet packet;
    packet.setGroupName(groupName);
    return QueueRequest(protocol::CreateGroupRequest, std::move(packet));
}

QString ConnectionGatewayClient::RequestJoinGroup(std::int64_t groupId, const QString& message)
{
    wimi::protocol::Packet packet;
    packet.setGroupId(groupId);
    packet.setRequestMessage(message);
    return QueueRequest(protocol::JoinGroupRequest, std::move(packet));
}

QString ConnectionGatewayClient::ReplyJoinGroup(std::int64_t groupId, std::int64_t requestorUid, bool accept)
{
    wimi::protocol::Packet packet;
    packet.setGroupId(groupId);
    packet.setRequestorUid(requestorUid);
    packet.setAccept(accept);
    return QueueRequest(protocol::ReplyJoinGroupRequest, std::move(packet));
}

QString ConnectionGatewayClient::PullGroupMembers(std::int64_t groupId)
{
    wimi::protocol::Packet packet;
    packet.setGroupId(groupId);
    return QueueRequest(protocol::PullGroupMembersRequest, std::move(packet));
}

QString ConnectionGatewayClient::QuitGroup(std::int64_t groupId)
{
    wimi::protocol::Packet packet;
    packet.setGroupId(groupId);
    return QueueRequest(protocol::QuitGroupRequest, std::move(packet));
}

QString ConnectionGatewayClient::KickGroupMember(std::int64_t groupId, std::int64_t targetUid)
{
    wimi::protocol::Packet packet;
    packet.setGroupId(groupId);
    packet.setRequestorUid(targetUid);
    return QueueRequest(protocol::KickGroupMemberRequest, std::move(packet));
}

QString ConnectionGatewayClient::SendGroupText(std::int64_t groupId, const QByteArray& utf8Text,
                                               const QString& clientMessageId, std::int64_t conversationId)
{
    wimi::protocol::Packet packet;
    packet.setGroupId(groupId);
    packet.setData(utf8Text);
    packet.setClientMessageId(clientMessageId);
    if (conversationId > 0) {
        packet.setConversationId(conversationId);
    }
    return QueueRequest(protocol::SendGroupTextRequest, std::move(packet));
}

QString ConnectionGatewayClient::SendGroupFile(std::int64_t groupId, const wimi::protocol::FileInfo& file,
                                               const QString& clientMessageId, std::int64_t conversationId)
{
    wimi::protocol::Packet packet;
    packet.setGroupId(groupId);
    packet.setFile(file);
    packet.setClientMessageId(clientMessageId);
    if (conversationId > 0)
        packet.setConversationId(conversationId);
    return QueueRequest(protocol::SendGroupFileRequest, std::move(packet));
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

QString ConnectionGatewayClient::QueueRequest(quint32 serviceId, wimi::protocol::Packet packet, int timeoutMilliseconds)
{
    const QString requestId = NewRequestId();
    if (!IsReady()) {
        emit RequestFailed(requestId, serviceId, protocol::DependencyUnavailable, tr("Connection Gateway 尚未就绪"),
                           false);
        return requestId;
    }

    packet.setRequestId(requestId);
    packet.setRequestTimeoutMs(kServerRequestBudgetMilliseconds);
    QByteArray payload;
    if (!SerializeProtobufPacket(packet, &payload)) {
        emit RequestFailed(requestId, serviceId, protocol::JsonParser, tr("无法序列化请求"), false);
        return requestId;
    }

    const quint32 responseServiceId = protocol::ResponseFor(serviceId);
    auto& queue = pending_by_response_[responseServiceId];
    queue.enqueue(PendingRequest{
        .requestId = requestId,
        .requestServiceId = serviceId,
        .responseServiceId = responseServiceId,
        .payload = payload,
        .timeoutMilliseconds = timeoutMilliseconds,
    });
    if (queue.size() == 1) {
        SendFront(responseServiceId);
    }
    return requestId;
}

void ConnectionGatewayClient::SendFront(quint32 responseServiceId)
{
    auto found = pending_by_response_.find(responseServiceId);
    if (found == pending_by_response_.end() || found->isEmpty() || !IsReady()) {
        return;
    }
    const PendingRequest& pending = found->head();
    socket_.write(TcpFrameCodec::Encode(pending.requestServiceId, pending.payload));

    QTimer* timer = request_timers_.value(responseServiceId);
    if (timer == nullptr) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        request_timers_.insert(responseServiceId, timer);
        connect(timer, &QTimer::timeout, this, [this, responseServiceId] {
            auto current = pending_by_response_.find(responseServiceId);
            if (current == pending_by_response_.end() || current->isEmpty()) {
                return;
            }
            const PendingRequest timedOut = current->dequeue();
            emit RequestFailed(timedOut.requestId, timedOut.requestServiceId, protocol::DeadlineExceeded,
                               tr("请求等待响应超时"), true);
            if (current->isEmpty()) {
                pending_by_response_.erase(current);
            } else {
                SendFront(responseServiceId);
            }
        });
    }
    timer->start(pending.timeoutMilliseconds);
}

void ConnectionGatewayClient::CompleteFront(quint32 responseServiceId, const QByteArray& payload)
{
    auto found = pending_by_response_.find(responseServiceId);
    if (found == pending_by_response_.end() || found->isEmpty()) {
        emit ProtocolError(tr("收到没有在途请求的响应 service=%1").arg(responseServiceId));
        return;
    }
    if (auto* timer = request_timers_.value(responseServiceId)) {
        timer->stop();
    }
    const PendingRequest completed = found->dequeue();
    emit ResponseReceived(completed.requestId, responseServiceId, payload);
    if (found->isEmpty()) {
        pending_by_response_.erase(found);
    } else {
        SendFront(responseServiceId);
    }
}

void ConnectionGatewayClient::FailAllPending(int errorCode, const QString& message, bool outcomeUnknown)
{
    for (auto* timer : std::as_const(request_timers_)) {
        timer->stop();
    }
    for (auto iterator = pending_by_response_.begin(); iterator != pending_by_response_.end(); ++iterator) {
        while (!iterator->isEmpty()) {
            const PendingRequest pending = iterator->dequeue();
            emit RequestFailed(pending.requestId, pending.requestServiceId, errorCode, message, outcomeUnknown);
        }
    }
    pending_by_response_.clear();
}

void ConnectionGatewayClient::SendReceipt(std::int64_t messageId, int receiptType, std::int64_t conversationId,
                                          std::int64_t conversationSequence)
{
    if (!IsReady() || messageId <= 0) {
        return;
    }
    wimi::protocol::Packet receipt;
    receipt.setSeq(messageId);
    receipt.setReceiptType(static_cast<wimi::protocol::ReceiptTypeGadget::ReceiptType>(receiptType));
    if (conversationId > 0 && conversationSequence > 0) {
        receipt.setConversationId(conversationId);
        receipt.setConversationSeq(conversationSequence);
    }
    SendPacket(protocol::Ack, receipt);
}

void ConnectionGatewayClient::SendPacket(quint32 serviceId, wimi::protocol::Packet packet)
{
    // QueueRequest already assigns a stable ID before serializing queued business
    // requests. Direct packets (login, ping, quit, and receipts) pass through
    // here, so make the Gateway's request_id invariant universal at this
    // boundary.
    if (!packet.hasRequestId() || packet.requestId().isEmpty()) {
        packet.setRequestId(NewRequestId());
    }
    QByteArray payload;
    if (!SerializeProtobufPacket(packet, &payload)) {
        emit ProtocolError(tr("无法序列化 service=%1").arg(serviceId));
        return;
    }
    socket_.write(TcpFrameCodec::Encode(serviceId, payload));
}

void ConnectionGatewayClient::HandleFrame(const TcpFrame& frame)
{
    if (frame.serviceId == protocol::LoginResponse) {
        HandleLoginResponse(frame.payload);
        return;
    }
    if (frame.serviceId == protocol::PingResponse) {
        return;
    }
    if (frame.serviceId == protocol::QuitResponse) {
        desired_open_ = false;
        socket_.disconnectFromHost();
        return;
    }

    wimi::protocol::Packet packet;
    if (!ParseProtobufPacket(frame.payload, &packet)) {
        emit ProtocolError(tr("无法解析 service=%1 的 Protobuf Packet").arg(frame.serviceId));
        socket_.abort();
        return;
    }
    if ((frame.serviceId & 1U) == 0U) {
        CompleteFront(frame.serviceId, frame.payload);
    } else {
        emit PushReceived(frame.serviceId, frame.payload);
    }
}

void ConnectionGatewayClient::HandleLoginResponse(const QByteArray& payload)
{
    wimi::protocol::Packet response;
    if (!ParseProtobufPacket(payload, &response)) {
        emit ProtocolError(tr("无法解析 Gateway 登录响应"));
        desired_open_ = false;
        socket_.abort();
        return;
    }
    const int errorCode = response.hasError() ? static_cast<int>(response.error()) : 0;
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
