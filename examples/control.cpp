// Runnable server/client example for health inspection and cooperative stop
// over gRPC. The example uses insecure credentials; deployments supply their
// own server policy and credentials.
#include "rtx/control.hpp"

#include <grpcpp/grpcpp.h>

#include <iostream>
#include <thread>
using namespace std::chrono_literals;
int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      std::cerr << "usage: rtx_control server|health|stop HOST:PORT\n";
      return 2;
    }
    const std::string command = argv[1], address = argv[2];
    if (command == "server") {
      rtx::Scheduler scheduler(
          {{"camera",
            20,
            10ms,
            8ms,
            100ms,
            {},
            [](const rtx::Context& c) { c.heartbeat(); }},
           {"perception",
            10,
            20ms,
            15ms,
            100ms,
            {"camera"},
            [](const rtx::Context& c) { c.heartbeat(); }}});
      rtx::PosixRunner runner(scheduler);
      rtx::ControlService service(scheduler, runner);
      grpc::ServerBuilder builder;
      builder.AddListeningPort(address, grpc::InsecureServerCredentials());
      builder.RegisterService(&service);
      auto server = builder.BuildAndStart();
      if (!server) throw std::runtime_error("could not bind server");
      std::cout << "control service listening on " << address << std::endl;
      try {
        runner.run();
      } catch (...) {
        server->Shutdown();
        throw;
      }
      server->Shutdown();
    } else if (command == "health" || command == "stop") {
      auto stub = rtx::rpc::Control::NewStub(
          grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + 3s);
      rtx::rpc::Empty request;
      grpc::Status status;
      if (command == "health") {
        rtx::rpc::HealthReport response;
        status = stub->GetHealth(&context, request, &response);
        if (status.ok())
          for (const auto& node : response.nodes())
            std::cout << node.name() << " " << node.state()
                      << " ticks=" << node.ticks()
                      << " deadline_misses=" << node.deadline_misses()
                      << " fault=" << node.fault() << '\n';
      } else {
        rtx::rpc::Empty response;
        status = stub->Stop(&context, request, &response);
      }
      if (!status.ok()) throw std::runtime_error(status.error_message());
    } else {
      throw std::invalid_argument("unknown command");
    }
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
