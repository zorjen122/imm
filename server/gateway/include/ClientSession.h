#pragma once

#include <cstdint>
#include <string>

namespace wimi::connection
{

class ClientSession
{
  public:
    virtual ~ClientSession() = default;

    virtual void Close() = 0;
    virtual bool SendRaw(std::string envelope) = 0;
    virtual bool SendReliable(std::string envelope, int64_t ackSeq) = 0;
    virtual const std::string& ConnectionId() const = 0;
};

} // namespace wimi::connection
