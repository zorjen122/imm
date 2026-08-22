#include "GatewayAcceptor.h"

#include "GatewaySession.h"
#include "Logger.h"
#include "MessageLinkManager.h"
#include "SessionManager.h"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>

namespace wimi::connection
{
namespace asio = boost::asio;

GatewayAcceptor::GatewayAcceptor(asio::io_context& ioContext, unsigned short port, SessionManager& sessionManager,
                                 MessageLinkManager& messageLinks, asio::thread_pool& businessPool)
    : ioContext(ioContext), acceptor(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)),
      manager(sessionManager), messageLinks(messageLinks), businessPool(businessPool)
{
    acceptor.set_option(asio::socket_base::reuse_address(true));
}

asio::awaitable<void> GatewayAcceptor::Run()
{
    asio::steady_timer readinessTimer(ioContext);
    while (!messageLinks.Ready()) {
        readinessTimer.expires_after(std::chrono::milliseconds(100));
        co_await readinessTimer.async_wait(asio::use_awaitable);
    }
    LOG_INFO(netLogger, "[gateway/accept] 开始接收客户端连接, healthy_message_links: {}",
             messageLinks.HealthyLinkCount());

    while (acceptor.is_open()) {
        boost::system::error_code ec;
        auto socket = co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            if (ec == asio::error::operation_aborted)
                co_return;
            LOG_WARN(netLogger, "[gateway/accept] 接收客户端连接失败, error: {}", ec.message());
            continue;
        }
        std::make_shared<GatewaySession>(std::move(socket), manager, messageLinks, businessPool)->Start();
    }
}

} // namespace wimi::connection
