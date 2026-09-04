#include "adapters/connection_gateway/TcpFrameCodec.h"

#include <QDataStream>
#include <QIODevice>

namespace wimi::client
{

QByteArray TcpFrameCodec::Encode(const QByteArray& payload)
{
    QByteArray frame;
    frame.reserve(kHeaderSize + payload.size());
    QDataStream stream(&frame, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << static_cast<quint32>(payload.size());
    frame.append(payload);
    return frame;
}

QVector<TcpFrame> TcpFrameCodec::Feed(const QByteArray& bytes)
{
    QVector<TcpFrame> frames;
    if (HasError()) {
        return frames;
    }

    buffer_.append(bytes);
    while (buffer_.size() >= static_cast<qsizetype>(kHeaderSize)) {
        QDataStream header(buffer_.left(kHeaderSize));
        header.setByteOrder(QDataStream::BigEndian);
        quint32 payloadSize = 0;
        header >> payloadSize;
        if (header.status() != QDataStream::Ok) {
            error_ = QStringLiteral("invalid TCP frame header");
            buffer_.clear();
            return {};
        }
        if (payloadSize > kMaximumPayloadSize) {
            error_ = QStringLiteral("TCP frame payload exceeds 10 MiB");
            buffer_.clear();
            return {};
        }

        const qsizetype totalSize = kHeaderSize + payloadSize;
        if (buffer_.size() < totalSize) {
            break;
        }

        frames.push_back(TcpFrame{
            .payload = buffer_.mid(kHeaderSize, payloadSize),
        });
        buffer_.remove(0, totalSize);
    }
    return frames;
}

bool TcpFrameCodec::HasError() const { return !error_.isEmpty(); }

QString TcpFrameCodec::ErrorString() const { return error_; }

void TcpFrameCodec::Reset()
{
    buffer_.clear();
    error_.clear();
}

} // namespace wimi::client
