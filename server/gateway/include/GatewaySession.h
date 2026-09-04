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
    bool SendRaw(std::string envelope);
    bool SendReliable(std::string envelope, int64_t ackSeq);
    const std::string& ConnectionId() const;

  private:
    struct ResultConnecctionRoute {
        int error{ErrorCodes::InternalError};
        TcpPacket response;
        db::SessionLease lease;
    };

    boost::asio::awaitable<void> Run();
    boost::asio::awaitable<void> HandlePacket(std::string payload);
    boost::asio::awaitable<bool> DirectProcess(protocol::Operation operation, const std::string& requestId,
                                               const TcpPacket& request, int64_t actor);
    boost::asio::awaitable<void> ForwardProcess(protocol::Operation operation, const std::string& requestId,
                                                const TcpPacket& request, int64_t actor);
    boost::asio::awaitable<void> WriteLoop();
    boost::asio::awaitable<bool> RefreshLeaseIfNeeded(int64_t actor);
    ResultConnecctionRoute HandleLoginConnection(TcpPacket request);
    void HandleForwardResult(const gateway::CommandResult& result, bool expectResponse, int64_t actor,
                             protocol::Operation operation, const std::string& requestId);
    void CloseInContext();
    void SendResponse(const std::string& requestId, protocol::Operation operation, TcpPacket packet);
    void SendError(const std::string& requestId, protocol::Operation operation, int error, const std::string& message);
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
        std::string envelope;
        unsigned int attempts{1};
        std::shared_ptr<boost::asio::steady_timer> timer;
    };
    std::unordered_map<int64_t, ReliableWrite> reliableWrites;
    std::chrono::steady_clock::time_point lastLeaseRefresh{};
    std::chrono::steady_clock::time_point rateWindowStarted{std::chrono::steady_clock::now()};
    unsigned int commandsInRateWindow{0};
};

} // namespace wimi::connection
