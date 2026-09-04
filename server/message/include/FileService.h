#pragma once

#include "TcpMessageCodec.h"

namespace wimi
{

class FileService
{
  public:
    TcpPacket Upload(int64_t actorUid, TcpPacket& request);
    TcpPacket Download(int64_t actorUid, TcpPacket& request);
};

} // namespace wimi
