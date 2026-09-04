#include "adapters/connection_gateway/ClientProtocol.h"
#include "adapters/connection_gateway/ConnectionGatewayClient.h"
#include "adapters/connection_gateway/ProtobufPacketCodec.h"
#include "adapters/connection_gateway/TcpFrameCodec.h"
#include "adapters/gate/GateHttpClient.h"

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

namespace wimi::client
{
namespace
{

wimi::protocol::Packet ParsePacket(const QByteArray& payload)
{
    wimi::protocol::Packet packet;
    const bool parsed = ParseProtobufPacket(payload, &packet);
    Q_ASSERT(parsed);
    return packet;
}

wimi::protocol::ClientEnvelope ParseEnvelope(const QByteArray& payload)
{
    wimi::protocol::ClientEnvelope envelope;
    const bool parsed = ParseClientEnvelope(payload, &envelope);
    Q_ASSERT(parsed);
    return envelope;
}

QByteArray PacketPayload(const wimi::protocol::Packet& packet)
{
    QByteArray payload;
    const bool serialized = SerializeProtobufPacket(packet, &payload);
    Q_ASSERT(serialized);
    return payload;
}

QByteArray ResponsePayload(const QString& requestId, wimi::protocol::OperationGadget::Operation operation,
                           wimi::protocol::Packet packet)
{
    wimi::protocol::ClientResponse response;
    response.setRequestId(requestId);
    response.setPacket(std::move(packet));
    response.setOperation(operation);
    wimi::protocol::ClientEnvelope envelope;
    envelope.setResponse(std::move(response));
    QByteArray payload;
    const bool serialized = SerializeClientEnvelope(envelope, &payload);
    Q_ASSERT(serialized);
    return payload;
}

QByteArray PushPayload(std::int64_t deliveryId, wimi::protocol::Packet packet)
{
    wimi::protocol::ClientPush push;
    push.setDeliveryId(deliveryId);
    push.setPacket(std::move(packet));
    push.setType(protocol::PushTypeFor(push.packet()));
    wimi::protocol::ClientEnvelope envelope;
    envelope.setPush(std::move(push));
    QByteArray payload;
    const bool serialized = SerializeClientEnvelope(envelope, &payload);
    Q_ASSERT(serialized);
    return payload;
}

void SetSuccess(wimi::protocol::Packet* packet)
{
    wimi::protocol::ResponseStatus status;
    status.setError(protocol::Success);
    packet->setStatus(std::move(status));
}

void WriteHttpJson(QTcpSocket* socket, const QJsonObject& object)
{
    const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const QByteArray response = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n") +
                                QByteArrayLiteral("Connection: close\r\nContent-Length: ") +
                                QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
    socket->write(response);
    socket->disconnectFromHost();
}

} // namespace

class ClientNetworkTest final : public QObject
{
    Q_OBJECT

  private slots:
    void qtProtobufMatchesCanonicalWireFormat();
    void optionalPacketTimestampDefaultsToEmpty();
    void gateSignInParsesGatewaySession();
    void gateCoversAccountRequests();
    void gatewayCoversSupportedRequestAndReceiptContracts();
    void gatewayReconnectsAndAuthenticatesAgain();
    void gatewayCanBeDestroyedWhileConnected();
};

void ClientNetworkTest::qtProtobufMatchesCanonicalWireFormat()
{
    wimi::protocol::UserInfo friendInfo;
    friendInfo.setUid(43);
    friendInfo.setName(QStringLiteral("Alice"));
    wimi::protocol::Packet packet;
    SetSuccess(&packet);
    wimi::protocol::PullFriendListResponse friends;
    friends.setFriends({friendInfo});
    packet.setPullFriendListResponse(std::move(friends));

    const auto parsed = ParsePacket(PacketPayload(packet));
    QVERIFY(parsed.hasStatus());
    QVERIFY(parsed.status().hasError());
    QCOMPARE(parsed.status().error(), 0);
    QVERIFY(parsed.hasPullFriendListResponse());
    QCOMPARE(parsed.pullFriendListResponse().friends().size(), 1);
    QCOMPARE(parsed.pullFriendListResponse().friends().constFirst().uid(), 43);
    QCOMPARE(parsed.pullFriendListResponse().friends().constFirst().name(), QStringLiteral("Alice"));
}

void ClientNetworkTest::optionalPacketTimestampDefaultsToEmpty()
{
    wimi::protocol::Packet packet;
    QCOMPARE(PacketSendDateTimeOrEmpty(packet), QString{});

    wimi::protocol::TextMessagePush push;
    push.setSendDateTime(QStringLiteral("2026-07-17 23:45:00"));
    packet.setTextMessagePush(std::move(push));
    QCOMPARE(PacketSendDateTimeOrEmpty(packet), QStringLiteral("2026-07-17 23:45:00"));
}

void ClientNetworkTest::gateSignInParsesGatewaySession()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QByteArray receivedRequest;
    connect(&server, &QTcpServer::newConnection, this, [&] {
        auto* socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [socket, &receivedRequest] {
            receivedRequest += socket->readAll();
            const int headerEnd = receivedRequest.indexOf("\r\n\r\n");
            if (headerEnd < 0 || !receivedRequest.contains(QByteArrayLiteral("zongjing"))) {
                return;
            }
            WriteHttpJson(socket, {{QStringLiteral("error"), 0},
                                   {QStringLiteral("uid"), 9001},
                                   {QStringLiteral("ip"), QStringLiteral("127.0.0.1")},
                                   {QStringLiteral("port"), 19090},
                                   {QStringLiteral("gatewayId"), QStringLiteral("gateway-a")},
                                   {QStringLiteral("chatToken"), QStringLiteral("secret")},
                                   {QStringLiteral("chatTokenExpiresIn"), 900},
                                   {QStringLiteral("init"), 1}});
        });
    });

    GateHttpClient gate;
    gate.SetBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())));
    QSignalSpy success(&gate, &GateHttpClient::SignInSucceeded);
    QSignalSpy failure(&gate, &GateHttpClient::OperationFailed);
    gate.SignIn(QStringLiteral("zongjing"), QStringLiteral("password"));

    QTRY_COMPARE(success.count(), 1);
    QCOMPARE(failure.count(), 0);
    QVERIFY(receivedRequest.startsWith(QByteArrayLiteral("POST /post-signIn")));
    QVERIFY(receivedRequest.contains(QByteArrayLiteral("\"password\":\"password\"")));

    const GateSession session = qvariant_cast<GateSession>(success.takeFirst().at(0));
    QCOMPARE(session.uid, 9001);
    QCOMPARE(session.gatewayHost, QStringLiteral("127.0.0.1"));
    QCOMPARE(session.gatewayPort, quint16(19090));
    QCOMPARE(session.gatewayId, QStringLiteral("gateway-a"));
    QCOMPARE(session.token, QStringLiteral("secret"));
    QCOMPARE(session.tokenExpiresInSeconds, 900);
    QCOMPARE(session.profileName, QStringLiteral("zongjing"));
    QVERIFY(session.profileInitializationRequired);
}

void ClientNetworkTest::gateCoversAccountRequests()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QList<QByteArray> requests;

    connect(&server, &QTcpServer::newConnection, this, [&] {
        auto* socket = server.nextPendingConnection();
        auto received = QSharedPointer<QByteArray>::create();
        connect(socket, &QTcpSocket::readyRead, socket, [socket, received, &requests] {
            *received += socket->readAll();
            if (!received->contains(QByteArrayLiteral("\r\n\r\n")) || !received->trimmed().endsWith('}')) {
                return;
            }
            requests.push_back(*received);
            WriteHttpJson(socket, {{QStringLiteral("error"), 0}});
        });
    });

    GateHttpClient gate;
    gate.SetBaseUrl(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())));
    QSignalSpy succeeded(&gate, &GateHttpClient::OperationSucceeded);
    QSignalSpy failed(&gate, &GateHttpClient::OperationFailed);

    gate.RequestVerificationCode(QStringLiteral("user@example.com"));
    QTRY_COMPARE(succeeded.count(), 1);
    gate.SignUp(QStringLiteral("user"), QStringLiteral("password"), QStringLiteral("user@example.com"),
                QStringLiteral("1234"));
    QTRY_COMPARE(succeeded.count(), 2);
    gate.ForgetPassword(QStringLiteral("user"), QStringLiteral("user@example.com"), QStringLiteral("1234"),
                        QStringLiteral("new-password"));
    QTRY_COMPARE(succeeded.count(), 3);

    QCOMPARE(failed.count(), 0);
    QCOMPARE(requests.size(), 3);
    QVERIFY(requests[0].startsWith(QByteArrayLiteral("POST /post-verifycode")));
    QVERIFY(requests[0].contains(QByteArrayLiteral("user@example.com")));
    QVERIFY(requests[1].startsWith(QByteArrayLiteral("POST /post-signUp")));
    QVERIFY(requests[1].contains(QByteArrayLiteral("\"verifycode\":\"1234\"")));
    QVERIFY(requests[2].startsWith(QByteArrayLiteral("POST /post-forget-password")));
    QVERIFY(requests[2].contains(QByteArrayLiteral("new-password")));
}

void ClientNetworkTest::gatewayCoversSupportedRequestAndReceiptContracts()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket* serverSocket = nullptr;
    TcpFrameCodec serverCodec;
    QVector<protocol::Operation> requestOperations;
    QHash<protocol::Operation, wimi::protocol::Packet> packets;
    QVector<wimi::protocol::Packet> receipts;

    connect(&server, &QTcpServer::newConnection, this, [&] {
        serverSocket = server.nextPendingConnection();
        connect(serverSocket, &QTcpSocket::readyRead, serverSocket, [&] {
            const auto frames = serverCodec.Feed(serverSocket->readAll());
            for (const auto& frame : frames) {
                const auto envelope = ParseEnvelope(frame.payload);
                QVERIFY(envelope.hasRequest());
                const auto& request = envelope.request();
                const auto& packet = request.packet();
                const auto operation = request.operation();
                if (operation == protocol::Operation::OP_LOGIN) {
                    QVERIFY(packet.hasLoginRequest());
                    QCOMPARE(packet.loginRequest().uid(), 42);
                    QCOMPARE(packet.loginRequest().authToken(), QStringLiteral("token-42"));
                    QVERIFY(!packet.loginRequest().deviceId().isEmpty());
                    wimi::protocol::Packet response;
                    SetSuccess(&response);
                    wimi::protocol::LoginResponse loginResponse;
                    loginResponse.setUid(42);
                    response.setLoginResponse(std::move(loginResponse));
                    serverSocket->write(TcpFrameCodec::Encode(
                        ResponsePayload(request.requestId(), request.operation(), std::move(response))));
                    continue;
                }
                if (operation == protocol::Operation::OP_QUIT) {
                    QVERIFY(packet.hasQuitRequest());
                    wimi::protocol::Packet response;
                    SetSuccess(&response);
                    response.setQuitResponse(wimi::protocol::QuitResponse{});
                    serverSocket->write(TcpFrameCodec::Encode(
                        ResponsePayload(request.requestId(), request.operation(), std::move(response))));
                    continue;
                }
                if (operation == protocol::Operation::OP_ACKNOWLEDGE) {
                    QVERIFY(packet.hasReceipt());
                    receipts.push_back(packet);
                    continue;
                }
                requestOperations.push_back(operation);
                packets.insert(operation, packet);
                wimi::protocol::Packet response;
                SetSuccess(&response);
                if (operation == protocol::Operation::OP_SEND_TEXT ||
                    operation == protocol::Operation::OP_SEND_GROUP_TEXT ||
                    operation == protocol::Operation::OP_SEND_GROUP_FILE) {
                    wimi::protocol::SendMessageResponse sendResponse;
                    sendResponse.setMessageId(7001);
                    sendResponse.setConversationId(8001);
                    sendResponse.setConversationSeq(9);
                    response.setSendMessageResponse(std::move(sendResponse));
                }
                serverSocket->write(TcpFrameCodec::Encode(
                    ResponsePayload(request.requestId(), request.operation(), std::move(response))));
            }
        });
    });

    ConnectionGatewayClient gateway;
    QSignalSpy authenticated(&gateway, &ConnectionGatewayClient::Authenticated);
    QSignalSpy responses(&gateway, &ConnectionGatewayClient::ResponseReceived);
    QSignalSpy pushes(&gateway, &ConnectionGatewayClient::PushReceived);
    QSignalSpy failures(&gateway, &ConnectionGatewayClient::RequestFailed);
    gateway.Open(GateSession{
        .uid = 42,
        .gatewayHost = QStringLiteral("127.0.0.1"),
        .gatewayPort = server.serverPort(),
        .gatewayId = QStringLiteral("local-gateway"),
        .token = QStringLiteral("token-42"),
        .tokenExpiresInSeconds = 900,
    });
    QTRY_COMPARE(authenticated.count(), 1);

    gateway.PullFriendList();
    gateway.PullFriendList();
    gateway.PullFriendApplications();
    gateway.PullConversationMessages(8001, 7, 60);
    gateway.SendFriendRequest(43, QStringLiteral("hello"));
    gateway.ReplyFriendRequest(43, true, QStringLiteral("welcome"));
    gateway.SendText(43, QByteArrayLiteral("text"), QStringLiteral("client-text"), 8001);
    wimi::protocol::FileInfo file;
    file.setFileId(QStringLiteral("5e3f43da-0d67-4e4a-99a0-6ccff21892a5"));
    file.setFileName(QStringLiteral("note.md"));
    file.setFileType(QStringLiteral("text/markdown"));
    file.setFileSize(4);
    file.setSha256(QByteArray(32, '\x5a'));
    gateway.UploadFileChunk(file, 0, {}, false);
    gateway.UploadFileChunk(file, 0, QByteArrayLiteral("file"), true);
    gateway.SendFile(43, file, QStringLiteral("client-file"), 8001);
    gateway.DownloadFileChunk(file, 0);
    gateway.CreateGroup(QStringLiteral("Qt group"));
    gateway.RequestJoinGroup(5001, QStringLiteral("join"));
    gateway.ReplyJoinGroup(5001, 43, true);
    gateway.PullGroupMembers(5001);
    gateway.QuitGroup(5001);
    gateway.KickGroupMember(5001, 43);
    gateway.SendGroupText(5001, QByteArrayLiteral("group text"), QStringLiteral("client-group"), 8100);
    gateway.SendGroupFile(5001, file, QStringLiteral("client-group-file"), 8100);

    QTRY_COMPARE(responses.count(), 19);
    QCOMPARE(failures.count(), 0);
    const QVector<protocol::Operation> expectedOperations = {
        protocol::Operation::OP_PULL_FRIEND_LIST,
        protocol::Operation::OP_PULL_FRIEND_APPLICATIONS,
        protocol::Operation::OP_PULL_CONVERSATION_MESSAGES,
        protocol::Operation::OP_ADD_FRIEND,
        protocol::Operation::OP_REPLY_FRIEND,
        protocol::Operation::OP_SEND_TEXT,
        protocol::Operation::OP_UPLOAD_FILE,
        protocol::Operation::OP_COMPLETE_FILE_UPLOAD,
        protocol::Operation::OP_SEND_FILE,
        protocol::Operation::OP_DOWNLOAD_FILE,
        protocol::Operation::OP_CREATE_GROUP,
        protocol::Operation::OP_JOIN_GROUP,
        protocol::Operation::OP_REPLY_JOIN_GROUP,
        protocol::Operation::OP_PULL_GROUP_MEMBERS,
        protocol::Operation::OP_QUIT_GROUP,
        protocol::Operation::OP_KICK_GROUP_MEMBER,
        protocol::Operation::OP_SEND_GROUP_TEXT,
        protocol::Operation::OP_SEND_GROUP_FILE,
    };
    for (const auto operation : expectedOperations) {
        QVERIFY2(requestOperations.contains(operation),
                 qPrintable(QStringLiteral("missing request operation %1").arg(static_cast<int>(operation))));
    }
    QCOMPARE(requestOperations.count(protocol::Operation::OP_PULL_FRIEND_LIST), 2);

    QCOMPARE(
        packets[protocol::Operation::OP_PULL_CONVERSATION_MESSAGES].pullConversationMessagesRequest().conversationId(),
        8001);
    QCOMPARE(packets[protocol::Operation::OP_PULL_CONVERSATION_MESSAGES].pullConversationMessagesRequest().afterSeq(),
             7);
    QCOMPARE(packets[protocol::Operation::OP_SEND_TEXT].sendTextRequest().to(), 43);
    QCOMPARE(packets[protocol::Operation::OP_SEND_TEXT].sendTextRequest().clientMessageId(),
             QStringLiteral("client-text"));
    QCOMPARE(packets[protocol::Operation::OP_SEND_GROUP_TEXT].sendGroupTextRequest().groupId(), 5001);
    QCOMPARE(packets[protocol::Operation::OP_KICK_GROUP_MEMBER].kickGroupMemberRequest().memberUid(), 43);
    QCOMPARE(packets[protocol::Operation::OP_SEND_GROUP_FILE].sendGroupFileRequest().file().fileId(), file.fileId());
    QCOMPARE(packets[protocol::Operation::OP_UPLOAD_FILE].fileUploadRequest().file().fileId(), file.fileId());
    QCOMPARE(packets[protocol::Operation::OP_COMPLETE_FILE_UPLOAD].fileUploadCompleteRequest().complete(), true);
    QCOMPARE(packets[protocol::Operation::OP_SEND_FILE].sendFileRequest().clientMessageId(),
             QStringLiteral("client-file"));
    QCOMPARE(packets[protocol::Operation::OP_DOWNLOAD_FILE].fileDownloadRequest().offset(), 0);

    wimi::protocol::Packet push;
    wimi::protocol::TextMessagePush textPush;
    textPush.setFrom(43);
    textPush.setTo(42);
    textPush.setContent(QByteArrayLiteral("incoming"));
    textPush.setMessageId(7002);
    textPush.setConversationId(8001);
    textPush.setConversationSeq(10);
    push.setTextMessagePush(std::move(textPush));
    serverSocket->write(TcpFrameCodec::Encode(PushPayload(7002, std::move(push))));
    QTRY_COMPARE(pushes.count(), 1);

    gateway.AcknowledgeDelivered(7002, 8001, 10);
    gateway.AcknowledgeRead(7002, 8001, 10);
    QTRY_COMPARE(receipts.size(), 2);
    QCOMPARE(receipts[0].receipt().receiptType(),
             wimi::protocol::ReceiptTypeGadget::ReceiptType::RECEIPT_TYPE_DELIVERED);
    QCOMPARE(receipts[0].receipt().conversationId(), 8001);
    QCOMPARE(receipts[0].receipt().conversationSeq(), 10);
    QCOMPARE(receipts[1].receipt().receiptType(), wimi::protocol::ReceiptTypeGadget::ReceiptType::RECEIPT_TYPE_READ);

    gateway.Close();
    QTRY_COMPARE(gateway.CurrentState(), ConnectionGatewayClient::State::Disconnected);
}

void ClientNetworkTest::gatewayReconnectsAndAuthenticatesAgain()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    int acceptedConnections = 0;
    QVector<wimi::protocol::Packet> loginPackets;

    connect(&server, &QTcpServer::newConnection, this, [&] {
        auto* socket = server.nextPendingConnection();
        ++acceptedConnections;
        auto codec = QSharedPointer<TcpFrameCodec>::create();
        connect(socket, &QTcpSocket::readyRead, socket, [socket, codec, &loginPackets] {
            for (const auto& frame : codec->Feed(socket->readAll())) {
                const auto envelope = ParseEnvelope(frame.payload);
                if (!envelope.hasRequest() || !envelope.request().packet().hasLoginRequest()) {
                    continue;
                }
                loginPackets.push_back(envelope.request().packet());
                wimi::protocol::Packet response;
                SetSuccess(&response);
                response.setLoginResponse(wimi::protocol::LoginResponse{});
                socket->write(TcpFrameCodec::Encode(ResponsePayload(
                    envelope.request().requestId(), envelope.request().operation(), std::move(response))));
            }
        });
    });

    ConnectionGatewayClient gateway;
    QSignalSpy authenticated(&gateway, &ConnectionGatewayClient::Authenticated);
    gateway.Open(GateSession{
        .uid = 55,
        .gatewayHost = QStringLiteral("127.0.0.1"),
        .gatewayPort = server.serverPort(),
        .token = QStringLiteral("token-55"),
        .tokenExpiresInSeconds = 900,
        .profileName = QStringLiteral("new-user"),
        .profileInitializationRequired = true,
    });
    QTRY_COMPARE(authenticated.count(), 1);
    QCOMPARE(acceptedConnections, 1);
    QCOMPARE(loginPackets.size(), 1);
    QVERIFY(loginPackets[0].loginRequest().initProfile());
    QCOMPARE(loginPackets[0].loginRequest().name(), QStringLiteral("new-user"));
    QVERIFY(!loginPackets[0].loginRequest().deviceId().isEmpty());

    const auto sockets = server.findChildren<QTcpSocket*>();
    QVERIFY(!sockets.isEmpty());
    sockets.front()->disconnectFromHost();
    QTRY_COMPARE_WITH_TIMEOUT(authenticated.count(), 2, 3000);
    QCOMPARE(acceptedConnections, 2);
    QCOMPARE(loginPackets.size(), 2);
    QVERIFY(!loginPackets[1].loginRequest().initProfile());
    QCOMPARE(loginPackets[1].loginRequest().deviceId(), loginPackets[0].loginRequest().deviceId());
    gateway.Close();
}

void ClientNetworkTest::gatewayCanBeDestroyedWhileConnected()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    {
        ConnectionGatewayClient gateway;
        gateway.Open(GateSession{
            .uid = 77,
            .gatewayHost = QStringLiteral("127.0.0.1"),
            .gatewayPort = server.serverPort(),
            .token = QStringLiteral("token-77"),
            .tokenExpiresInSeconds = 900,
        });
        QTRY_COMPARE(gateway.CurrentState(), ConnectionGatewayClient::State::Authenticating);
    }
}

} // namespace wimi::client

QTEST_GUILESS_MAIN(wimi::client::ClientNetworkTest)

#include "ClientNetworkTest.moc"
