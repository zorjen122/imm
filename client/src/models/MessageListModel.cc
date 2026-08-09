#include "models/MessageListModel.h"

#include <QDate>

#include <algorithm>
#include <utility>

namespace wimi::client
{
namespace
{

QDate MessageDate(const QString& timestamp)
{
    if (timestamp.size() < 10) {
        return {};
    }
    return QDate::fromString(timestamp.left(10), QStringLiteral("yyyy-MM-dd"));
}

QString MessageTimeText(const QString& timestamp)
{
    const QDate date = MessageDate(timestamp);
    if (!date.isValid()) {
        return timestamp;
    }
    return timestamp.size() >= 16 ? timestamp.mid(11, 5) : timestamp;
}

QString MessageDateLabel(const QString& timestamp)
{
    const QDate date = MessageDate(timestamp);
    if (!date.isValid()) {
        if (timestamp == QStringLiteral("昨天") || timestamp.startsWith(u'周')) {
            return timestamp;
        }
        return QStringLiteral("今天");
    }

    const QDate today = QDate::currentDate();
    if (date == today) {
        return QStringLiteral("今天");
    }
    if (date == today.addDays(-1)) {
        return QStringLiteral("昨天");
    }
    if (date.year() == today.year()) {
        return QStringLiteral("%1月%2日").arg(date.month()).arg(date.day());
    }
    return QStringLiteral("%1年%2月%3日").arg(date.year()).arg(date.month()).arg(date.day());
}

} // namespace

QString DeliveryStateName(MessageDeliveryState state)
{
    switch (state) {
    case MessageDeliveryState::PendingLocal:
        return QStringLiteral("pending");
    case MessageDeliveryState::WaitingAccept:
        return QStringLiteral("waiting-accept");
    case MessageDeliveryState::Accepted:
        return QStringLiteral("accepted");
    case MessageDeliveryState::Delivered:
        return QStringLiteral("delivered");
    case MessageDeliveryState::Read:
        return QStringLiteral("read");
    case MessageDeliveryState::Unknown:
        return QStringLiteral("unknown");
    case MessageDeliveryState::RetryableFailed:
        return QStringLiteral("retryable-failed");
    case MessageDeliveryState::PermanentFailed:
        return QStringLiteral("permanent-failed");
    }
    return QStringLiteral("unknown");
}

bool CanTransition(MessageDeliveryState from, MessageDeliveryState to)
{
    if (from == to) {
        return false;
    }

    switch (from) {
    case MessageDeliveryState::PendingLocal:
        return to == MessageDeliveryState::WaitingAccept || to == MessageDeliveryState::Unknown ||
               to == MessageDeliveryState::RetryableFailed || to == MessageDeliveryState::PermanentFailed;
    case MessageDeliveryState::WaitingAccept:
        return to == MessageDeliveryState::Accepted || to == MessageDeliveryState::Unknown ||
               to == MessageDeliveryState::RetryableFailed || to == MessageDeliveryState::PermanentFailed;
    case MessageDeliveryState::Accepted:
        return to == MessageDeliveryState::Delivered || to == MessageDeliveryState::Read;
    case MessageDeliveryState::Delivered:
        return to == MessageDeliveryState::Read;
    case MessageDeliveryState::Unknown:
        return to == MessageDeliveryState::WaitingAccept || to == MessageDeliveryState::Accepted ||
               to == MessageDeliveryState::Delivered || to == MessageDeliveryState::Read ||
               to == MessageDeliveryState::RetryableFailed || to == MessageDeliveryState::PermanentFailed;
    case MessageDeliveryState::RetryableFailed:
        return to == MessageDeliveryState::PendingLocal || to == MessageDeliveryState::WaitingAccept ||
               to == MessageDeliveryState::Accepted || to == MessageDeliveryState::PermanentFailed;
    case MessageDeliveryState::Read:
    case MessageDeliveryState::PermanentFailed:
        return false;
    }
    return false;
}

MessageListModel::MessageListModel(QObject* parent) : QAbstractListModel(parent) {}

int MessageListModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : records_.size(); }

QVariant MessageListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= records_.size()) {
        return {};
    }

    const auto& record = records_.at(index.row());
    switch (role) {
    case ClientMessageIdRole:
        return QVariant::fromValue<qlonglong>(record.clientMessageId);
    case MessageIdRole:
        return record.messageId.has_value() ? QVariant::fromValue<qlonglong>(*record.messageId) : QVariant{};
    case ConversationSeqRole:
        return record.conversationSeq.has_value() ? QVariant::fromValue<qlonglong>(*record.conversationSeq)
                                                  : QVariant{};
    case SenderIdRole:
        return record.senderId;
    case BodyRole:
        return record.body;
    case TimestampRole:
        return MessageTimeText(record.timestamp);
    case SourceIndexRole:
        return index.row();
    case DateLabelRole:
        return MessageDateLabel(record.timestamp);
    case ShowDateSeparatorRole:
        return index.row() == 0 ||
               MessageDateLabel(records_[index.row() - 1].timestamp) != MessageDateLabel(record.timestamp);
    case ShowUnreadSeparatorRole:
        return index.row() == unread_separator_index_;
    case OutgoingRole:
        return record.outgoing;
    case DeliveryStateRole:
        return DeliveryStateName(record.deliveryState);
    case TypeRole:
        return record.type;
    case FileIdRole:
        return record.fileId;
    case FileNameRole:
        return record.fileName;
    case FileTypeRole:
        return record.fileType;
    case FileSizeRole:
        return QVariant::fromValue<qlonglong>(record.fileSize);
    case FileExpiredRole:
        return record.fileExpired;
    default:
        return {};
    }
}

QHash<int, QByteArray> MessageListModel::roleNames() const
{
    return {
        {ClientMessageIdRole, "clientMessageId"},
        {MessageIdRole, "messageId"},
        {ConversationSeqRole, "conversationSeq"},
        {SenderIdRole, "senderId"},
        {BodyRole, "body"},
        {TimestampRole, "timestamp"},
        {SourceIndexRole, "sourceIndex"},
        {DateLabelRole, "dateLabel"},
        {ShowDateSeparatorRole, "showDateSeparator"},
        {ShowUnreadSeparatorRole, "showUnreadSeparator"},
        {OutgoingRole, "outgoing"},
        {DeliveryStateRole, "deliveryState"},
        {TypeRole, "messageType"},
        {FileIdRole, "fileId"},
        {FileNameRole, "fileName"},
        {FileTypeRole, "fileType"},
        {FileSizeRole, "fileSize"},
        {FileExpiredRole, "fileExpired"},
    };
}

void MessageListModel::SetRecords(QVector<MessageRecord> records, int unreadCount)
{
    beginResetModel();
    records_ = std::move(records);
    if (unreadCount >= 0) {
        const int boundedUnreadCount = std::min(unreadCount, static_cast<int>(records_.size()));
        unread_separator_index_ = boundedUnreadCount > 0 ? static_cast<int>(records_.size()) - boundedUnreadCount : -1;
    }
    endResetModel();
}

void MessageListModel::Append(MessageRecord record)
{
    const int row = records_.size();
    beginInsertRows({}, row, row);
    records_.push_back(std::move(record));
    endInsertRows();
}

bool MessageListModel::UpdateDeliveryState(std::int64_t clientMessageId, MessageDeliveryState state)
{
    for (int row = 0; row < records_.size(); ++row) {
        auto& record = records_[row];
        if (record.clientMessageId != clientMessageId) {
            continue;
        }
        if (!CanTransition(record.deliveryState, state)) {
            return false;
        }
        record.deliveryState = state;
        const auto modelIndex = index(row);
        emit dataChanged(modelIndex, modelIndex, {DeliveryStateRole});
        return true;
    }
    return false;
}

} // namespace wimi::client
