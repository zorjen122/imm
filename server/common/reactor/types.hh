#pragma once
#include <cstdint>
#include <string>

namespace wimi::io
{
struct endpoint {
    std::string host;
    uint16_t port;
};
struct mysql_options {
    endpoint address{"127.0.0.1", 3306};
    std::string user;
    std::string password;
    std::string schema;
    std::string ca_file;
    std::string ca_pem;
};
} // namespace wimi::io
