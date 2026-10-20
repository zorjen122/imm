#pragma once
#include "io/operations.hh"
#include "io/options.hh"
#include <seastar/core/iostream.hh>
#include <seastar/net/tls.hh>
#include <optional>

namespace imm::gate_io
{
// Owns a single SMTP transaction. The caller keeps this object alive through
// send(), abort if necessary, and close(); all socket work stays on its shard.
class smtp_connection
{
    seastar::socket _dialer;
    std::optional<seastar::connected_socket> _socket;
    std::optional<seastar::input_stream<char>> _input;
    std::optional<seastar::output_stream<char>> _output;
    bool _aborted{};
    bool _accepted{};
    void streams();
    seastar::future<int> reply();
    seastar::future<> command(std::string text, int expected);

  public:
    smtp_connection(const options&, seastar::shared_ptr<seastar::tls::certificate_credentials>);
    seastar::future<> send(seastar::net::inet_address address, const options&, send_verification_email,
                           seastar::shared_ptr<seastar::tls::certificate_credentials>);
    void abort();
    seastar::future<> close();
};
} // namespace imm::gate_io
