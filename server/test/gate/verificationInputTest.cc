#include "VerificationService.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

void Require(bool condition, const std::string& message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

int main()
{
    Require(wimi::VerificationService::NormalizeEmail("  User@Example.COM \n") == "user@example.com",
            "email normalization failed");
    Require(wimi::VerificationService::IsValidEmail("user@example.com"), "valid email was rejected");
    Require(!wimi::VerificationService::IsValidEmail("missing-at.example.com"), "email without @ was accepted");
    Require(!wimi::VerificationService::IsValidEmail("user@example"), "email without a domain suffix was accepted");
    Require(!wimi::VerificationService::IsValidEmail("user@exa{mple.com"),
            "email with Redis hash-tag delimiters was accepted");
    return EXIT_SUCCESS;
}
