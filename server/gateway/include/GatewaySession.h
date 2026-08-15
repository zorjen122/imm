#pragma once

#include "Redis.h"
#include "TcpMessageCodec.h"

#include <boost/asio.hpp>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

namespace wimi::gateway
{
class CommandResult;
}

namespace wimi::connection
{

class MessageLinkManager;
class SessionManager;

class GatewaySession : public std::enable_shared_from_this<GatewaySession>
{
  public:
    GatewaySession(boost::asio::ip::tcp::socket socket, SessionManager& registry, MessageLinkManager& messageLinks,
                   boost::asio::thread_pool& businessPool);

    void Start();
    void Close();
    bool SendRaw(std::string packet, uint32_t protocolId);
    bool SendReliable(std::string packet, uint32_t protocolId, int64_t ackSeq);
    const std::string& ConnectionId() const;

  private:
    struct ResultConnecctionRoute {
        int error{ErrorCodes::InternalError};
        TcpPacket response;
        db::SessionLease lease;
    };

    boost::asio::awaitable<void> Run();
    boost::asio::awaitable<void> HandlePacket(uint32_t protocolId, std::string payload);
    boost::asio::awaitable<void> WriteLoop();
    boost::asio::awaitable<bool> RefreshLeaseIfNeeded(int64_t actor);
    ResultConnecctionRoute RequestConnectionRoute(TcpPacket request);
    void HandleForwardResult(const gateway::CommandResult& result, bool expectResponse, int64_t actor,
                             uint32_t protocolId, const std::string& requestId);
    void CloseInContext();
    void SendError(uint32_t requestId, int error, const std::string& message);
    void ArmReliableWrite(int64_t ackSeq);
    void AcknowledgeDelivery(int64_t ackSeq);

    boost::asio::ip::tcp::socket socket;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    SessionManager& registry;
    MessageLinkManager& messageLinks;
    boost::asio::thread_pool& businessPool;
    std::string connectionId;
    std::atomic<int64_t> userId{0};
    db::SessionLease lease;
    std::atomic<bool> closed{false};
    std::atomic<std::size_t> queuedWrites{0};
    std::deque<std::shared_ptr<std::string>> writeQueue;
    bool writeActive{false};
    bool closeAfterWrite{false};
    struct ReliableWrite {
        std::string packet;
        uint32_t protocolId{0};
        unsigned int attempts{1};
        std::shared_ptr<boost::asio::steady_timer> timer;
    };
    std::unordered_map<int64_t, ReliableWrite> reliableWrites;
    std::chrono::steady_clock::time_point lastLeaseRefresh{};
    std::chrono::steady_clock::time_point rateWindowStarted{std::chrono::steady_clock::now()};
    unsigned int commandsInRateWindow{0};
};

} // namespace wimi::connection
