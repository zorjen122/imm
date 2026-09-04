#pragma once

#include "TcpMessageCodec.h"

#include <string>
#include <vector>

namespace wimi
{

class MessageService
{
  public:
    struct AcceptedText {
        TcpPacket response;
        TcpPacket forwardPacket;
        int64_t recipientUid{0};
        bool shouldForward{false};
    };

    struct AcceptedGroupText {
        TcpPacket response;
        TcpPacket forwardPacket;
        std::vector<int64_t> recipientUids;
        bool shouldForward{false};
    };
    using AcceptedFile = AcceptedText;

    struct AckResult {
        TcpPacket response;
        TcpPacket readReceipt;
        int64_t senderUid{0};
        bool shouldForwardRead{false};
    };

    AcceptedText AcceptText(int64_t actorUid, const TcpPacket& request);
    AcceptedGroupText AcceptGroupText(int64_t actorUid, const TcpPacket& request);
    AcceptedFile AcceptFile(int64_t actorUid, const TcpPacket& request);
    AcceptedGroupText AcceptGroupFile(int64_t actorUid, const TcpPacket& request);

    AckResult Ack(int64_t actorUid, const TcpPacket& request, const std::string& deviceId);
    TcpPacket PullConversationMessages(int64_t actorUid, TcpPacket& request);
};

} // namespace wimi
