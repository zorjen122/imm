#include "api/email.hh"

#include <algorithm>
#include <cctype>

namespace imm::gate_protocol
{
std::string normalize_email(const std::string& email)
{
    auto first = std::find_if_not(email.begin(), email.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    auto last =
        std::find_if_not(email.rbegin(), email.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
    if (first >= last)
        return {};

    std::string normalized(first, last);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized;
}

bool is_valid_email(const std::string& email)
{
    if (email.empty() || email.size() > 254 || email.find_first_of(" \t\r\n{}") != std::string::npos)
        return false;
    const auto at = email.find('@');
    if (at == std::string::npos || at == 0 || at != email.rfind('@') || at + 3 > email.size())
        return false;
    const auto dot = email.find('.', at + 2);
    return dot != std::string::npos && dot + 1 < email.size();
}

} // namespace imm::gate_protocol
