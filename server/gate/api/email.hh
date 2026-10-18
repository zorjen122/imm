#pragma once
#include <string>

namespace wimi::gate_protocol
{
// Trim surrounding whitespace and lowercase the email before validation and
// Redis key construction. Valid input excludes whitespace and hash-tag braces.
std::string normalize_email(const std::string& email);
bool is_valid_email(const std::string& email);
} // namespace wimi::gate_protocol
