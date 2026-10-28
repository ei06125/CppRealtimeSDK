#include "control_service.hpp"

#include <chrono>
#include <thread>

namespace rtsdk {
namespace {

// Maps the SDK status struct onto the protobuf message.
void fill(const NodeStatus& s, v1::NodeStatus* o) {
  o->set_name(s.name);
  o->set_state(to_string(s.state));
  o->set_priority(s.priority);
  o->set_period_us(s.period_us);
  o->set_restarts(s.restarts);
  auto* st = o->mutable_stats();
  st->set_cycles(s.stats.cycles);
  st->set_deadline_misses(s.stats.deadline_misses);
  st->set_skipped_releases(s.stats.skipped_releases);
  st->set_step_failures(s.stats.step_failures);
  st->set_max_exec_us(s.stats.max_exec_us);
  st->set_max_latency_us(s.stats.max_latency_us);
  st->set_total_exec_us(s.stats.total_exec_us);
}

}  // namespace

grpc::Status ControlService::GetStatus(grpc::ServerContext*, const v1::Empty*,
                                       v1::ManagerStatus* out) {
  out->set_state(to_string(mgr_.state()));
  out->set_node_count(static_cast<std::uint32_t>(mgr_.nodes().size()));
  return grpc::Status::OK;
}

grpc::Status ControlService::ListNodes(grpc::ServerContext*, const v1::Empty*,
                                       v1::NodeList* out) {
  for (const auto& n : mgr_.nodes()) fill(n, out->add_nodes());
  return grpc::Status::OK;
}

grpc::Status ControlService::RestartNode(grpc::ServerContext*,
                                         const v1::NodeRequest* in,
                                         v1::CommandResult* out) {
  const bool ok = mgr_.restart_node(in->name());
  out->set_ok(ok);
  out->set_message(
      ok ? "restarted"
         : "unknown node, manager not running, or node unresponsive");
  return grpc::Status::OK;
}

grpc::Status ControlService::StopNode(grpc::ServerContext*,
                                      const v1::NodeRequest* in,
                                      v1::CommandResult* out) {
  const bool ok = mgr_.stop_node(in->name());
  out->set_ok(ok);
  out->set_message(ok ? "stopped" : "unknown node or node unresponsive");
  return grpc::Status::OK;
}

// Server-streaming: replays history after `after_seq`, then polls for new
// events every 100 ms until the client cancels. Polling keeps the manager free
// of any gRPC dependency.
grpc::Status ControlService::StreamEvents(
    grpc::ServerContext* ctx, const v1::EventsRequest* in,
    grpc::ServerWriter<v1::Event>* writer) {
  std::uint64_t cursor = in->after_seq();
  while (!ctx->IsCancelled()) {
    for (const auto& e : mgr_.events_since(cursor)) {
      v1::Event ev;
      ev.set_seq(e.seq);
      ev.set_at_us(e.at_us);
      ev.set_kind(e.kind);
      ev.set_node(e.node);
      ev.set_detail(e.detail);
      if (!writer->Write(ev)) return grpc::Status::OK;
      cursor = e.seq;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return grpc::Status::OK;
}

// Convenience wrapper; pass port 0 in `address` and read selected_port for
// tests.
std::unique_ptr<grpc::Server> start_control_server(ControlService& svc,
                                                   const std::string& address,
                                                   int* selected_port) {
  grpc::ServerBuilder b;
  b.AddListeningPort(address, grpc::InsecureServerCredentials(),
                     selected_port);  // put mTLS in front for production
  b.RegisterService(&svc);
  return b.BuildAndStart();
}

}  // namespace rtsdk
