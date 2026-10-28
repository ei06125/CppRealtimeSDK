// End-to-end gRPC test: real server on a loopback port, real client stub.
#include <chrono>
#include <thread>

#include "../tests/rt_test.hpp"
#include "../tests/test_support.hpp"
#include "control_service.hpp"

using namespace testutil;

namespace {

struct Rig {
  SimPlatform plat;
  ExecutionManager mgr{plat, ExecutionManager::Mode::Tick};
  std::unique_ptr<ControlService> svc;
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<v1::ExecutionControl::Stub> stub;

  Rig() {
    mgr.register_factory("ok", [](const NodeConfig&) {
      return std::make_unique<LambdaNode>([](const StepContext&) { return true; });
    });
    LaunchConfig c;
    std::string err;
    LaunchConfig::parse("node a type=ok priority=100 period_ms=10\nnode b type=ok priority=50 period_ms=20\n"
                        "start a\nstart b after=a\n", c, err);
    mgr.configure(c, err);
    mgr.start(err, false);
    drive(plat, *mgr.tick_scheduler(), 100 * kMs);
    svc = std::make_unique<ControlService>(mgr);
    int port = 0;
    server = start_control_server(*svc, "127.0.0.1:0", &port);
    stub = v1::ExecutionControl::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  }
  ~Rig() { server->Shutdown(); }
};

}  // namespace

TEST(grpc_status_and_list_nodes) {
  Rig r;
  grpc::ClientContext ctx;
  v1::Empty e;
  v1::ManagerStatus st;
  CHECK(r.stub->GetStatus(&ctx, e, &st).ok());
  CHECK(st.state() == "running");
  CHECK_EQ(st.node_count(), 2u);

  grpc::ClientContext ctx2;
  v1::NodeList list;
  CHECK(r.stub->ListNodes(&ctx2, e, &list).ok());
  CHECK_EQ(list.nodes_size(), 2);
  CHECK(list.nodes(0).name() == "a" && list.nodes(0).priority() == 100);
  CHECK(list.nodes(0).state() == "running");
  CHECK(list.nodes(0).stats().cycles() >= 9);
}

TEST(grpc_restart_and_stop_commands) {
  Rig r;
  v1::NodeRequest req;
  v1::CommandResult res;
  req.set_name("a");
  grpc::ClientContext c1;
  CHECK(r.stub->RestartNode(&c1, req, &res).ok() && res.ok());
  grpc::ClientContext c2;
  CHECK(r.stub->StopNode(&c2, req, &res).ok() && res.ok());
  NodeStatus a;
  CHECK(r.mgr.node("a", a) && a.state == NodeState::Stopped && a.restarts == 1);
  req.set_name("missing");
  grpc::ClientContext c3;
  CHECK(r.stub->StopNode(&c3, req, &res).ok() && !res.ok());
}

TEST(grpc_event_stream_delivers_history_and_new_events) {
  Rig r;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  v1::EventsRequest req;
  auto reader = r.stub->StreamEvents(&ctx, req);
  v1::Event ev;
  int launch = 0;
  while (launch < 2 && reader->Read(&ev))
    if (ev.kind() == "launch") ++launch;
  CHECK_EQ(launch, 2);
  r.mgr.stop_node("b");  // new event after the stream started
  bool saw = false;
  while (!saw && reader->Read(&ev)) saw = (ev.kind() == "command" && ev.node() == "b");
  CHECK(saw);
  ctx.TryCancel();
}
