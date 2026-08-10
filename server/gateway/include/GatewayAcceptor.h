#pragma once

#include <boost/asio.hpp>

namespace wimi::connection
{

class MessageLinkManager;
class SessionManager;

class GatewayAcceptor
{
  public:
    GatewayAcceptor(boost::asio::io_context& ioContext, unsigned short port, SessionManager& registry,
                  MessageLinkManager& messageLinks, boost::asio::thread_pool& businessPool);

    boost::asio::awaitable<void> Run();

  private:
    boost::asio::io_context& ioContext;
    boost::asio::ip::tcp::acceptor acceptor;
    SessionManager& manager;
    MessageLinkManager& messageLinks;
    boost::asio::thread_pool& businessPool;
};

} // namespace wimi::connection
