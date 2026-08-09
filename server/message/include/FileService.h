#pragma once

#include "TcpMessageCodec.h"

namespace wimi
{

class FileService
{
  public:
    TcpPacket Upload(uint32_t msgID, TcpPacket& request);
    TcpPacket Download(uint32_t msgID, TcpPacket& request);
};

} // namespace wimi
