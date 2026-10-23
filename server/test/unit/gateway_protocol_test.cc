#include "api/protocol.hh"
#include <iostream>
#include <set>
#include <stdexcept>

namespace
{
void require(bool value, const char* message)
{
    if (!value) {
        throw std::runtime_error(message);
    }
}
} // namespace
int main()
{
    using namespace imm;
    std::set<int> registered;
    for (const auto& route : gateway_protocol::routes()) {
        require(route.handle != nullptr && registered.insert(route.operation).second,
                "missing or duplicate API handler");
    }
    const auto* operations = protocol::Operation_descriptor();
    for (int index = 0; index < operations->value_count(); ++index) {
        const auto value = operations->value(index)->number();
        if (value != protocol::OP_UNSPECIFIED) {
            require(registered.contains(value), "protobuf operation not covered by Gateway API registry");
        }
    }
    ClientEnvelope envelope;
    auto* request = envelope.mutable_request();
    request->set_request_id("probe");
    request->set_operation(protocol::OP_PING);
    request->mutable_packet()->mutable_ping_request();
    require(gateway_protocol::decode(envelope.SerializeAsString()).request().request_id() == "probe",
            "valid frame rejected");
    request->set_operation(protocol::OP_SEND_TEXT);
    bool invalid{};
    try {
        gateway_protocol::decode(envelope.SerializeAsString());
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    require(invalid, "mismatched operation/payload accepted");
    request->set_operation(protocol::OP_LOGIN);
    auto* login = request->mutable_packet()->mutable_login_request();
    login->set_uid(1001);
    login->set_auth_token("token");
    require(gateway_protocol::validate(*request) == ErrorCodes::TokenInvalid, "missing device accepted");
    login->set_device_id("device");
    require(gateway_protocol::validate(*request) == 0, "valid login rejected");
    login->set_init_profile(true);
    login->set_age(32768);
    require(gateway_protocol::validate(*request) != 0, "profile storage bound not checked");
    std::cout << "PASS all Gateway API operations registered, protobuf validation and login bounds\n";
}
