#pragma once

#include "tcp_message.qpb.h"

#include <QByteArray>
#include <QString>

namespace wimi::client
{

bool SerializeProtobufPacket(const wimi::protocol::Packet& packet, QByteArray* payload);
bool ParseProtobufPacket(const QByteArray& payload, wimi::protocol::Packet* packet);
bool SerializeClientEnvelope(const wimi::protocol::ClientEnvelope& envelope, QByteArray* payload);
bool ParseClientEnvelope(const QByteArray& payload, wimi::protocol::ClientEnvelope* envelope);
QString PacketSendDateTimeOrEmpty(const wimi::protocol::Packet& packet);

} // namespace wimi::client
