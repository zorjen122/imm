#include "io/smtp_connection.hh"
#include <openssl/evp.h>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <stdexcept>

namespace imm::gate_io
{
namespace
{
seastar::tls::tls_options tls_options(const std::string& host)
{
    seastar::tls::tls_options result;
    result.server_name = host;
    return result;
}
std::string base64(const std::string& text)
{
    std::string encoded(4 * ((text.size() + 2) / 3) + 1, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                     reinterpret_cast<const unsigned char*>(text.data()), text.size());
    encoded.resize(size);
    return encoded;
}
} // namespace
smtp_connection::smtp_connection(const options& config,
                                 seastar::shared_ptr<seastar::tls::certificate_credentials> credentials)
    : _dialer(config.implicit_tls ? seastar::tls::socket(std::move(credentials), tls_options(config.smtp.host))
                                  : seastar::engine().net().socket())
{
}
void smtp_connection::streams()
{
    _input.emplace(_socket->input());
    _output.emplace(_socket->output());
}
seastar::future<int> smtp_connection::reply()
{
    int status{};
    for (unsigned index = 0; index < 32; ++index) {
        std::string line;
        while (line.size() < 4096) {
            const auto byte = co_await _input->read_exactly(1);
            if (byte.empty()) {
                throw std::runtime_error("SMTP peer disconnected");
            }
            line.push_back(byte[0]);
            if (byte[0] == '\n') {
                break;
            }
        }
        if (line.size() < 5 || !line.ends_with("\r\n") || line[0] < '2' || line[0] > '5' || line[1] < '0' ||
            line[1] > '9' || line[2] < '0' || line[2] > '9') {
            throw std::runtime_error("invalid SMTP response");
        }
        const int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + line[2] - '0';
        if (status && status != code) {
            throw std::runtime_error("inconsistent SMTP multiline response");
        }
        status = code;
        if (line[3] == ' ') {
            co_return status;
        }
        if (line[3] != '-') {
            throw std::runtime_error("invalid SMTP continuation");
        }
    }
    throw std::runtime_error("SMTP response too long");
}
seastar::future<> smtp_connection::command(std::string text, int expected)
{
    co_await _output->write(text);
    co_await _output->flush();
    const int status = co_await reply();
    if (status != expected) {
        throw std::runtime_error("SMTP rejected command, status=" + std::to_string(status));
    }
}
seastar::future<> smtp_connection::send(seastar::net::inet_address address, const options& config,
                                        send_verification_email mail,
                                        seastar::shared_ptr<seastar::tls::certificate_credentials> credentials)
{
    _socket.emplace(co_await _dialer.connect(seastar::socket_address(address, config.smtp.port)));
    streams();
    if (co_await reply() != 220) {
        throw std::runtime_error("SMTP greeting rejected");
    }
    co_await command("EHLO imm\r\n", 250);
    if (!config.implicit_tls) {
        co_await command("STARTTLS\r\n", 220);
        // STARTTLS 改变同一连接的传输层：先释放明文流，再移交 socket 给 TLS，
        // 握手后重建流并重新 EHLO，认证信息仅在受保护的连接上发送。
        _input.reset();
        _output.reset();
        auto raw = std::move(*_socket);
        _socket.reset();
        auto secured =
            co_await seastar::tls::wrap_client(std::move(credentials), std::move(raw), tls_options(config.smtp.host));
        _socket.emplace(std::move(secured));
        streams();
        co_await command("EHLO imm\r\n", 250);
    }
    co_await command("AUTH LOGIN\r\n", 334);
    co_await command(base64(config.smtp_user) + "\r\n", 334);
    co_await command(base64(config.smtp_password) + "\r\n", 235);
    co_await command("MAIL FROM:<" + config.smtp_user + ">\r\n", 250);
    co_await command("RCPT TO:<" + mail.recipient + ">\r\n", 250);
    co_await command("DATA\r\n", 354);
    co_await command("To: " + mail.recipient + "\r\nFrom: " + config.smtp_user +
                         "\r\nSubject: IMM verification code\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
                         "Your IMM verification code is " +
                         mail.code + ". It expires in " + std::to_string(mail.ttl_seconds) + " seconds.\r\n.\r\n",
                     250);
    // DATA 最终的 250 表示 SMTP 服务端已接受邮件，不代表收件人已收到。
    // 后续 QUIT 断连不能撤销接受结果，也不能让业务清理该邮件中的有效验证码。
    _accepted = true;
    try {
        co_await command("QUIT\r\n", 221);
    } catch (...) {
        abort();
    }
}
void smtp_connection::abort()
{
    _aborted = true;
    _dialer.shutdown();
    if (_socket) {
        _socket->shutdown_input();
        _socket->shutdown_output();
    }
}
seastar::future<> smtp_connection::close()
{
    if (_output) {
        try {
            co_await _output->close();
        } catch (...) {
            if (!_aborted && !_accepted) {
                throw;
            }
        }
        _output.reset();
    }
    if (_input) {
        try {
            co_await _input->close();
        } catch (...) {
            if (!_aborted && !_accepted) {
                throw;
            }
        }
        _input.reset();
    }
    _socket.reset();
}
} // namespace imm::gate_io
