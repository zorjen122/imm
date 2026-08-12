#pragma once

#include "TcpMessageCodec.h"

namespace wimi
{

class ClientForwardService;

class FriendService
{
  public:
    explicit FriendService(ClientForwardService& clientForwardService);

    TcpPacket NotifyAddFriend(unsigned int msgID, int64_t actorUid, TcpPacket& request);
    TcpPacket ReplyAddFriend(unsigned int msgID, int64_t actorUid, TcpPacket& request);
    TcpPacket PullFriendApplyList(uint32_t msgID, int64_t actorUid, TcpPacket& request);
    TcpPacket PullFriendList(uint32_t msgID, int64_t actorUid, TcpPacket& request);

  private:
    int StoreNotifyAddFriend(int64_t actorUid, const protocol::AddFriendRequest& request);
    int StoreReplyAddFriend(int64_t actorUid, const protocol::ReplyFriendRequest& request);

    ClientForwardService& clientForwardService;
};

} // namespace wimi
