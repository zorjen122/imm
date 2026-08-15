#pragma once
#include <string>

namespace wimi
{

struct ServiceNodeInfo {
    std::string id;
    std::string host;
    unsigned short port{0};
    std::string status{"active"};
    unsigned int weight{1};

    bool active() const { return status == "active" && !host.empty() && port != 0; }
};
}; // namespace wimi
