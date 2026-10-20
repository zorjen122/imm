#include "state.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <iostream>
#include <thread>

namespace
{
class state_fixture final : public state::StateService::Service
{
    std::chrono::milliseconds _delay;

  public:
    explicit state_fixture(unsigned delay) : _delay(delay) {}
    grpc::Status PickConnectionGateway(grpc::ServerContext* context, const state::ConnectUser* request,
                                       state::ConnectUserRsp* response) override
    {
        std::cout << "call " << request->id() << '\n' << std::flush;
        const auto end = std::chrono::steady_clock::now() + _delay;
        while (std::chrono::steady_clock::now() < end && !context->IsCancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        response->set_ip("127.0.0.1");
        response->set_port(8090);
        response->set_node_id("io-test-gateway");
        return grpc::Status::OK;
    }
};
} // namespace
int main(int argc, char** argv)
{
    if (argc != 3) {
        return 2;
    }
    state_fixture service(std::stoul(argv[2]));
    grpc::ServerBuilder builder;
    builder.AddListeningPort(std::string("127.0.0.1:") + argv[1], grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server) {
        return 1;
    }
    std::cout << "ready\n" << std::flush;
    server->Wait();
}
