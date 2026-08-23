#include "GrpcSecurity.h"

#include <yaml-cpp/yaml.h>

#include <exception>
#include <iostream>

int main()
{
    auto development = YAML::Load(R"(
transportSecurity:
  environment: development
  mode: insecure
)");
    auto developmentConfig = wimi::LoadGrpcSecurityConfig(development);
    if (developmentConfig.Mtls() || !wimi::BuildChannelCredentials(developmentConfig)) {
        std::cerr << "development insecure credentials were rejected\n";
        return 1;
    }

    auto production = YAML::Load(R"(
transportSecurity:
  environment: production
  mode: insecure
)");
    try {
        static_cast<void>(wimi::LoadGrpcSecurityConfig(production));
    } catch (const std::exception&) {
        return 0;
    }
    std::cerr << "production insecure credentials were accepted\n";
    return 1;
}
