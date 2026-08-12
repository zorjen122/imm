#pragma once

#include "TcpMessageCodec.h"

#include <cstdint>
#include <string>

namespace wimi
{

class ClientForwardService;

class GroupService
{
  public:
    explicit GroupService(ClientForwardService& clientForwardService);

    TcpPacket Create(unsigned int msgID, int64_t actorUid, TcpPacket& request);
    TcpPacket NotifyJoin(unsigned int msgID, int64_t actorUid, TcpPacket& request);
    TcpPacket PullNotify(unsigned int msgID, int64_t actorUid, TcpPacket& request);
    TcpPacket ReplyJoin(unsigned int msgID, int64_t actorUid, TcpPacket& request);
    TcpPacket Quit(unsigned int msgID, int64_t actorUid, TcpPacket& request);
    TcpPacket Kick(unsigned int msgID, int64_t actorUid, TcpPacket& request);

  private:
    void NotifyJoinRequest(int64_t masterUid, int64_t requestorUid, int64_t groupId, const std::string& requestMessage);
    void NotifyChange(const std::vector<int64_t>& recipients, int64_t groupId, int64_t targetUid, int64_t actorUid,
                      protocol::GroupChangeKind change, const std::string& groupName, bool accept = false);

    ClientForwardService& clientForwardService;
};

} // namespace wimi
