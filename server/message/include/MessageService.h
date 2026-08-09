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

    AcceptedText AcceptText(TcpPacket request);
    AcceptedGroupText AcceptGroupText(TcpPacket request);
    AcceptedFile AcceptFile(TcpPacket request);

    AckResult Ack(TcpPacket& request, const std::string& deviceId);
    TcpPacket SendText(uint32_t msgID, TcpPacket& request);
    TcpPacket SendFile(uint32_t msgID, TcpPacket& request);
    TcpPacket SendGroupText(uint32_t msgID, TcpPacket& request);
    TcpPacket PullConversationMessages(uint32_t msgID, TcpPacket& request);
};

} // namespace wimi
