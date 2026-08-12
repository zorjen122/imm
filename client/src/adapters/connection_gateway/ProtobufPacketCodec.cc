#include "adapters/connection_gateway/ProtobufPacketCodec.h"

#include <QProtobufSerializer>

namespace wimi::client
{

bool SerializeProtobufPacket(const wimi::protocol::Packet& packet, QByteArray* payload)
{
    if (payload == nullptr) {
        return false;
    }
    QProtobufSerializer serializer;
    *payload = packet.serialize(&serializer);
    return serializer.lastError() == QAbstractProtobufSerializer::Error::None;
}

bool ParseProtobufPacket(const QByteArray& payload, wimi::protocol::Packet* packet)
{
    if (packet == nullptr) {
        return false;
    }
    QProtobufSerializer serializer;
    return packet->deserialize(&serializer, payload);
}

QString PacketSendDateTimeOrEmpty(const wimi::protocol::Packet& packet)
{
    return packet.hasTextMessagePush() ? packet.textMessagePush().sendDateTime() : QString{};
}

} // namespace wimi::client
