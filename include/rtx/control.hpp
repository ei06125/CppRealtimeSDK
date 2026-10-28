// gRPC management and binary-channel services hosted by the application.
// Services borrow their scheduler, runner and channel objects; those objects must outlive requests.
#pragma once
#include "rtx/posix.hpp"
#include "rtx/shared_memory.hpp"
#include <unordered_map>
#include "control.grpc.pb.h"
namespace rtx {
// Host this on a management thread. Server ownership and credentials belong to the application.
class ControlService final : public rpc::Control::Service {
 public:
 ControlService(Scheduler& scheduler,PosixRunner& runner):scheduler_(scheduler),stop_([&runner]{runner.request_stop();}){}
 ControlService(Scheduler& scheduler,PosixTaskRunner& runner):scheduler_(scheduler),stop_([&runner]{runner.request_stop();}){}
 grpc::Status GetHealth(grpc::ServerContext*,const rpc::Empty*,rpc::HealthReport*) override;
 grpc::Status Stop(grpc::ServerContext*,const rpc::Empty*,rpc::Empty*) override;
 private:
 Scheduler& scheduler_;
 std::function<void()> stop_;
};
// Registry is fixed at construction; referenced mailboxes must outlive the service.
// Channel names are application aliases, not shared-memory filesystem names.
class MailboxService final : public rpc::Mailbox::Service {
 public:
 explicit MailboxService(std::unordered_map<std::string,SharedMemory*> channels);
 grpc::Status Read(grpc::ServerContext*,const rpc::Channel*,rpc::Payload*) override;
 grpc::Status Write(grpc::ServerContext*,const rpc::Payload*,rpc::Empty*) override;
 private:
 const std::unordered_map<std::string,SharedMemory*> channels_;
};
}
