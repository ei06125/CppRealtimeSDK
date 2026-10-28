// Demonstrate external binary channels feeding a configured node through shared
// memory. The XOR transform is deliberately simple so clients can verify the
// full data path.
#include <grpcpp/grpcpp.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>

#include "rtx/control.hpp"
#include "rtx/launch.hpp"
using namespace std::chrono_literals;
int main(int argc, char** argv) {
  try {
    if (argc < 4 || argc > 5)
      throw std::invalid_argument(
          "usage: rtx_pipeline server ADDRESS CONFIG | read ADDRESS CHANNEL | "
          "write ADDRESS CHANNEL TEXT");
    const std::string command = argv[1], address = argv[2];
    if (command == "server" && argc == 4) {
      const auto base = "/rtx-demo-" + std::to_string(getpid());
      struct Cleanup {
        std::string a, b;
        ~Cleanup() {
          shm_unlink(a.c_str());
          shm_unlink(b.c_str());
        }
      } cleanup{base + "-in", base + "-out"};
      rtx::SharedMemory input(cleanup.a, 256, true),
          output(cleanup.b, 256, true);
      rtx::CallbackRegistry callbacks{
          {"transform", [&](const rtx::Context& c) {
             std::array<std::byte, 256> buffer{};
             const auto size = input.read(buffer);
             if (size) {
               for (std::size_t i = 0; i < size; ++i)
                 buffer[i] ^= std::byte{0x55};
               output.write(std::span(buffer).first(size));
             }
             c.heartbeat();
           }}};
      std::ifstream config(argv[3]);
      if (!config) throw std::runtime_error("cannot open launch configuration");
      rtx::Scheduler scheduler(rtx::load_launch(config, callbacks));
      rtx::PosixTaskRunner runner(scheduler, false);
      rtx::ControlService control(scheduler, runner);
      rtx::MailboxService channels({{"input", &input}, {"output", &output}});
      grpc::ServerBuilder builder;
      builder.AddListeningPort(address, grpc::InsecureServerCredentials());
      builder.RegisterService(&control);
      builder.RegisterService(&channels);
      auto server = builder.BuildAndStart();
      if (!server) throw std::runtime_error("server bind failed");
      std::cout << "pipeline listening on " << address << std::endl;
      try {
        runner.run();
      } catch (...) {
        server->Shutdown();
        throw;
      }
      server->Shutdown();
    } else if ((command == "read" && argc == 4) ||
               (command == "write" && argc == 5)) {
      auto stub = rtx::rpc::Mailbox::NewStub(
          grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + 3s);
      grpc::Status status;
      if (command == "read") {
        rtx::rpc::Channel request;
        request.set_name(argv[3]);
        rtx::rpc::Payload response;
        status = stub->Read(&context, request, &response);
        if (status.ok()) {
          for (unsigned char byte : response.data())
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(byte);
          std::cout << '\n';
        }
      } else {
        rtx::rpc::Payload request;
        request.set_channel(argv[3]);
        request.set_data(argv[4]);
        rtx::rpc::Empty response;
        status = stub->Write(&context, request, &response);
      }
      if (!status.ok()) throw std::runtime_error(status.error_message());
    } else
      throw std::invalid_argument("invalid command or argument count");
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
