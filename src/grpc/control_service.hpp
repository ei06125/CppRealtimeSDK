// gRPC adapter for ExecutionManager. Built only when gRPC is available
// (CMake -DRTSDK_ENABLE_GRPC=ON, Bazel //:rtsdk_grpc).
#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "execution_control.grpc.pb.h"
#include "rtsdk/manager.hpp"

namespace rtsdk {

class ControlService final : public v1::ExecutionControl::Service {
 public:
  explicit ControlService(ExecutionManager& mgr) : mgr_(mgr) {}

  grpc::Status GetStatus(grpc::ServerContext*, const v1::Empty*, v1::ManagerStatus* out) override;
  grpc::Status ListNodes(grpc::ServerContext*, const v1::Empty*, v1::NodeList* out) override;
  grpc::Status RestartNode(grpc::ServerContext*, const v1::NodeRequest*, v1::CommandResult* out) override;
  grpc::Status StopNode(grpc::ServerContext*, const v1::NodeRequest*, v1::CommandResult* out) override;
  grpc::Status StreamEvents(grpc::ServerContext*, const v1::EventsRequest*,
                            grpc::ServerWriter<v1::Event>* writer) override;

 private:
  ExecutionManager& mgr_;
};

// Starts a server on `address` (e.g. "0.0.0.0:50051", or "127.0.0.1:0" for any
// free port, reported through `selected_port`); caller owns shutdown.
std::unique_ptr<grpc::Server> start_control_server(ControlService& svc, const std::string& address,
                                                   int* selected_port = nullptr);

}  // namespace rtsdk
