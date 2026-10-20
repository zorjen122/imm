#include "api/protocol.hh"
#include "transport/connection.hh"
#include <array>
#include <stdexcept>

namespace wimi::gateway_protocol
{
namespace
{
const std::array registry{
    route{protocol::OP_LOGIN, processing::login, &gateway_transport::connection::login},
    route{protocol::OP_PING, processing::immediate, &gateway_transport::connection::ping},
    route{protocol::OP_QUIT, processing::immediate, &gateway_transport::connection::quit},
    route{protocol::OP_ACKNOWLEDGE, processing::immediate, &gateway_transport::connection::acknowledge_request},
    route{protocol::OP_PULL_FRIEND_LIST, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_PULL_FRIEND_APPLICATIONS, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_PULL_CONVERSATION_MESSAGES, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_PULL_GROUP_MEMBERS, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_UPDATE_USER_INFO, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_SEARCH_USER, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_ADD_FRIEND, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_REPLY_FRIEND, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_REMOVE_FRIEND, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_SEND_TEXT, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_SEND_FILE, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_UPLOAD_FILE, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_CREATE_GROUP, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_JOIN_GROUP, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_REPLY_JOIN_GROUP, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_QUIT_GROUP, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_SEND_GROUP_TEXT, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_UPLOAD_FILE_CHUNK, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_COMPLETE_FILE_UPLOAD, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_DOWNLOAD_FILE, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_KICK_GROUP_MEMBER, processing::forward, &gateway_transport::connection::forward},
    route{protocol::OP_SEND_GROUP_FILE, processing::forward, &gateway_transport::connection::forward},
};
}
std::span<const route> routes() { return registry; }
const route& route_for(protocol::Operation operation)
{
    for (const route& endpoint : registry) {
        if (endpoint.operation == operation) {
            return endpoint;
        }
    }
    throw std::invalid_argument("unregistered Gateway operation");
}
int validate(const protocol::ClientRequest& request)
{
    if (request.operation() == protocol::OP_LOGIN) {
        const auto& login = request.packet().login_request();
        if (login.uid() <= 0 || login.device_id().empty() || login.device_id().size() > 36 ||
            login.device_id().find_first_of("\r\n") != std::string::npos || login.auth_token().empty()) {
            return ErrorCodes::TokenInvalid;
        }
        if (login.init_profile() && (login.name().size() > 64 || login.sex().size() > 16 ||
                                     login.head_image_url().size() > 512 || login.age() < 0 || login.age() > 32767)) {
            return ErrorCodes::ProtobufPacket;
        }
    }
    return 0;
}
ClientEnvelope decode(std::string_view payload)
{
    ClientEnvelope envelope;
    if (!envelope.ParseFromArray(payload.data(), payload.size()) || !envelope.has_request() ||
        envelope.request().request_id().empty() ||
        !TcpRequestMatchesOperation(envelope.request().operation(), envelope.request().packet())) {
        throw std::invalid_argument("invalid Gateway protobuf request");
    }
    route_for(envelope.request().operation());
    return envelope;
}
} // namespace wimi::gateway_protocol
