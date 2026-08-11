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

QByteArray PacketPayload(const wimi::protocol::Packet& packet)
{
    QByteArray payload;
    const bool serialized = SerializeProtobufPacket(packet, &payload);
    Q_ASSERT(serialized);
    return payload;
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
    const QByteArray canonicalWire =
        QByteArray::fromHex("082a2a0568656c6c6f3800ba0208746f6b656e2d3432c002d936c80201ea0209"
                            "726571756573742d31f002c13ef80209820308636c69656e742d31920309082b"
                            "1205416c696365");

    wimi::protocol::UserInfo friendInfo;
    friendInfo.setUid(43);
    friendInfo.setName(QStringLiteral("Alice"));
    wimi::protocol::Packet packet;
    packet.setUid(42);
    packet.setData(QByteArrayLiteral("hello"));
    packet.setError(0);
    packet.setAuthToken(QStringLiteral("token-42"));
    packet.setMessageId(7001);
    packet.setMessageState(wimi::protocol::MessageStateGadget::MessageState::MESSAGE_STATE_ACCEPTED);
    packet.setRequestId(QStringLiteral("request-1"));
    packet.setConversationId(8001);
    packet.setConversationSeq(9);
    packet.setClientMessageId(QStringLiteral("client-1"));
    packet.setFriendList({friendInfo});

    QCOMPARE(PacketPayload(packet), canonicalWire);

    const auto parsed = ParsePacket(canonicalWire);
    QCOMPARE(parsed.uid(), 42);
    QCOMPARE(parsed.data(), QByteArrayLiteral("hello"));
    QVERIFY(parsed.hasError());
    QCOMPARE(parsed.error(), 0);
    QCOMPARE(parsed.authToken(), QStringLiteral("token-42"));
    QCOMPARE(parsed.messageId(), 7001);
    QCOMPARE(parsed.messageState(), wimi::protocol::MessageStateGadget::MessageState::MESSAGE_STATE_ACCEPTED);
    QCOMPARE(parsed.requestId(), QStringLiteral("request-1"));
    QCOMPARE(parsed.conversationId(), 8001);
    QCOMPARE(parsed.conversationSeq(), 9);
    QCOMPARE(parsed.clientMessageId(), QStringLiteral("client-1"));
    QCOMPARE(parsed.friendList().size(), 1);
    QCOMPARE(parsed.friendList().constFirst().uid(), 43);
    QCOMPARE(parsed.friendList().constFirst().name(), QStringLiteral("Alice"));
}

void ClientNetworkTest::optionalPacketTimestampDefaultsToEmpty()
{
    wimi::protocol::Packet packet;
    QCOMPARE(PacketSendDateTimeOrEmpty(packet), QString{});

    packet.setSendDateTime(QStringLiteral("2026-07-17 23:45:00"));
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
    QVector<quint32> requestServices;
    QHash<quint32, wimi::protocol::Packet> packets;
    QVector<wimi::protocol::Packet> receipts;

    connect(&server, &QTcpServer::newConnection, this, [&] {
        serverSocket = server.nextPendingConnection();
        connect(serverSocket, &QTcpSocket::readyRead, serverSocket, [&] {
            const auto frames = serverCodec.Feed(serverSocket->readAll());
            for (const auto& frame : frames) {
                const auto packet = ParsePacket(frame.payload);
                if (frame.serviceId == protocol::LoginRequest) {
                    QCOMPARE(packet.uid(), 42);
                    QCOMPARE(packet.authToken(), QStringLiteral("token-42"));
                    QVERIFY(!packet.deviceId().isEmpty());
                    QVERIFY(!packet.requestId().isEmpty());
                    wimi::protocol::Packet response;
                    response.setError(protocol::Success);
                    response.setUid(42);
                    serverSocket->write(TcpFrameCodec::Encode(protocol::LoginResponse, PacketPayload(response)));
                    continue;
                }
                if (frame.serviceId == protocol::QuitRequest) {
                    QVERIFY(!packet.requestId().isEmpty());
                    wimi::protocol::Packet response;
                    response.setError(protocol::Success);
                    serverSocket->write(TcpFrameCodec::Encode(protocol::QuitResponse, PacketPayload(response)));
                    continue;
                }
                if (frame.serviceId == protocol::Ack) {
                    QVERIFY(!packet.requestId().isEmpty());
                    receipts.push_back(packet);
                    continue;
                }
                requestServices.push_back(frame.serviceId);
                packets.insert(frame.serviceId, packet);
                wimi::protocol::Packet response;
                response.setError(protocol::Success);
                if (frame.serviceId == protocol::SendTextRequest || frame.serviceId == protocol::SendGroupTextRequest ||
                    frame.serviceId == protocol::SendGroupFileRequest) {
                    response.setClientMessageId(packet.clientMessageId());
                    response.setMessageId(7001);
                    response.setConversationId(8001);
                    response.setConversationSeq(9);
                    response.setMessageState(wimi::protocol::MessageStateGadget::MessageState::MESSAGE_STATE_ACCEPTED);
                }
                serverSocket->write(
                    TcpFrameCodec::Encode(protocol::ResponseFor(frame.serviceId), PacketPayload(response)));
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
    const QVector<quint32> expectedServices = {
        protocol::PullFriendListRequest,  protocol::PullFriendApplyListRequest, protocol::PullSessionMessagesRequest,
        protocol::AddFriendRequest,       protocol::ReplyFriendRequest,         protocol::SendTextRequest,
        protocol::UploadFileRequest,      protocol::FileUploadCompleteRequest,  protocol::SendFileRequest,
        protocol::FileDownloadRequest,    protocol::CreateGroupRequest,         protocol::JoinGroupRequest,
        protocol::ReplyJoinGroupRequest,  protocol::PullGroupMembersRequest,    protocol::QuitGroupRequest,
        protocol::KickGroupMemberRequest, protocol::SendGroupTextRequest,       protocol::SendGroupFileRequest,
    };
    for (const quint32 service : expectedServices) {
        QVERIFY2(requestServices.contains(service),
                 qPrintable(QStringLiteral("missing request service %1").arg(service)));
    }
    QCOMPARE(requestServices.count(protocol::PullFriendListRequest), 2);

    QCOMPARE(packets[protocol::PullSessionMessagesRequest].conversationId(), 8001);
    QCOMPARE(packets[protocol::PullSessionMessagesRequest].afterSeq(), 7);
    QCOMPARE(packets[protocol::SendTextRequest].to(), 43);
    QCOMPARE(packets[protocol::SendTextRequest].clientMessageId(), QStringLiteral("client-text"));
    QCOMPARE(packets[protocol::SendGroupTextRequest].groupId(), 5001);
    QCOMPARE(packets[protocol::KickGroupMemberRequest].requestorUid(), 43);
    QCOMPARE(packets[protocol::SendGroupFileRequest].file().fileId(), file.fileId());
    QCOMPARE(packets[protocol::UploadFileRequest].file().fileId(), file.fileId());
    QCOMPARE(packets[protocol::FileUploadCompleteRequest].fileComplete(), true);
    QCOMPARE(packets[protocol::SendFileRequest].clientMessageId(), QStringLiteral("client-file"));
    QCOMPARE(packets[protocol::FileDownloadRequest].fileOffset(), 0);
    for (const quint32 service : expectedServices) {
        QVERIFY(!packets[service].requestId().isEmpty());
        QCOMPARE(packets[service].requestTimeoutMs(), 10'000U);
    }

    wimi::protocol::Packet push;
    push.setFrom(43);
    push.setTo(42);
    push.setData(QByteArrayLiteral("incoming"));
    push.setSeq(7002);
    push.setMessageId(7002);
    push.setConversationId(8001);
    push.setConversationSeq(10);
    serverSocket->write(TcpFrameCodec::Encode(protocol::SendTextRequest, PacketPayload(push)));
    QTRY_COMPARE(pushes.count(), 1);

    gateway.AcknowledgeDelivered(7002, 8001, 10);
    gateway.AcknowledgeRead(7002, 8001, 10);
    QTRY_COMPARE(receipts.size(), 2);
    QCOMPARE(receipts[0].receiptType(), wimi::protocol::ReceiptTypeGadget::ReceiptType::RECEIPT_TYPE_DELIVERED);
    QCOMPARE(receipts[0].conversationId(), 8001);
    QCOMPARE(receipts[0].conversationSeq(), 10);
    QCOMPARE(receipts[1].receiptType(), wimi::protocol::ReceiptTypeGadget::ReceiptType::RECEIPT_TYPE_READ);

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
                if (frame.serviceId != protocol::LoginRequest) {
                    continue;
                }
                loginPackets.push_back(ParsePacket(frame.payload));
                wimi::protocol::Packet response;
                response.setError(protocol::Success);
                socket->write(TcpFrameCodec::Encode(protocol::LoginResponse, PacketPayload(response)));
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
    QVERIFY(loginPackets[0].init());
    QCOMPARE(loginPackets[0].name(), QStringLiteral("new-user"));
    QVERIFY(!loginPackets[0].deviceId().isEmpty());

    const auto sockets = server.findChildren<QTcpSocket*>();
    QVERIFY(!sockets.isEmpty());
    sockets.front()->disconnectFromHost();
    QTRY_COMPARE_WITH_TIMEOUT(authenticated.count(), 2, 3000);
    QCOMPARE(acceptedConnections, 2);
    QCOMPARE(loginPackets.size(), 2);
    QVERIFY(!loginPackets[1].init());
    QCOMPARE(loginPackets[1].deviceId(), loginPackets[0].deviceId());
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
