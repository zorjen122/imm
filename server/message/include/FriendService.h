#pragma once

#include "TcpMessageCodec.h"

namespace wimi
{

class ClientForwardService;

class FriendService
{
  public:
    explicit FriendService(ClientForwardService& clientForwardService);

    TcpPacket NotifyAddFriend(int64_t actorUid, TcpPacket& request);
    TcpPacket ReplyAddFriend(int64_t actorUid, TcpPacket& request);
    TcpPacket PullFriendApplyList(int64_t actorUid, TcpPacket& request);
    TcpPacket PullFriendList(int64_t actorUid, TcpPacket& request);

  private:
    int StoreNotifyAddFriend(int64_t actorUid, const protocol::AddFriendRequest& request);
    int StoreReplyAddFriend(int64_t actorUid, const protocol::ReplyFriendRequest& request);

    ClientForwardService& clientForwardService;
};

} // namespace wimi
