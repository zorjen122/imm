#pragma once

#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <yaml-cpp/yaml.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace wimi
{

struct GrpcSecurityConfig {
    std::string environment{"development"};
    std::string mode{"insecure"};
    std::string rootCertificates;
    std::string certificateChain;
    std::string privateKey;

    bool Mtls() const { return mode == "mtls"; }
};

inline std::string ReadCredentialFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open gRPC credential file: " + path);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

inline GrpcSecurityConfig LoadGrpcSecurityConfig(const YAML::Node& server)
{
    GrpcSecurityConfig result;
    auto source = server["transportSecurity"];
    if (source) {
        if (source["environment"])
            result.environment = source["environment"].as<std::string>();
        if (source["mode"])
            result.mode = source["mode"].as<std::string>();
        if (source["rootCertificates"])
            result.rootCertificates = source["rootCertificates"].as<std::string>();
        if (source["certificateChain"])
            result.certificateChain = source["certificateChain"].as<std::string>();
        if (source["privateKey"])
            result.privateKey = source["privateKey"].as<std::string>();
    }
    if (result.environment == "production" && result.mode != "mtls")
        throw std::runtime_error("production Gateway-Message transport requires mTLS");
    if (result.mode != "insecure" && result.mode != "mtls")
        throw std::runtime_error("unsupported gRPC transport security mode: " + result.mode);
    if (result.Mtls() &&
        (result.rootCertificates.empty() || result.certificateChain.empty() || result.privateKey.empty()))
        throw std::runtime_error("mTLS requires rootCertificates, certificateChain and privateKey");
    return result;
}

inline std::shared_ptr<grpc::ChannelCredentials> BuildChannelCredentials(const GrpcSecurityConfig& config)
{
    if (!config.Mtls())
        return grpc::InsecureChannelCredentials();
    grpc::SslCredentialsOptions options;
    options.pem_root_certs = ReadCredentialFile(config.rootCertificates);
    options.pem_cert_chain = ReadCredentialFile(config.certificateChain);
    options.pem_private_key = ReadCredentialFile(config.privateKey);
    return grpc::SslCredentials(options);
}

inline std::shared_ptr<grpc::ServerCredentials> BuildServerCredentials(const GrpcSecurityConfig& config)
{
    if (!config.Mtls())
        return grpc::InsecureServerCredentials();
    grpc::SslServerCredentialsOptions options;
    options.pem_root_certs = ReadCredentialFile(config.rootCertificates);
    options.pem_key_cert_pairs.push_back(
        {ReadCredentialFile(config.privateKey), ReadCredentialFile(config.certificateChain)});
    options.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
    return grpc::SslServerCredentials(options);
}

} // namespace wimi
