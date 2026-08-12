#pragma once

#include "TcpMessageCodec.h"

namespace wimi
{

class FileService
{
  public:
    TcpPacket Upload(uint32_t msgID, int64_t actorUid, TcpPacket& request);
    TcpPacket Download(uint32_t msgID, int64_t actorUid, TcpPacket& request);
};

} // namespace wimi
