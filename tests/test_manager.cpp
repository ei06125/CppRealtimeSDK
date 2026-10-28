// ExecutionManager tests: launch sequencing, fault policies, commands, events.
// Time is simulated (ManualClock) so every outcome is deterministic.
#include <atomic>
#include <map>

#include "rt_test.hpp"
#include "rtsdk/manager.hpp"
#include "test_support.hpp"

using namespace testutil;

namespace {

// Test node behaviours selected by factory type.
struct Probe {
  std::vector<std::string> log;                    // "<node>:init" / "<node>:step"
  std::map<std::string, TimeUs> first_release;
  std::map<std::string, int> inits;
  int fail_inits_for_bad = 1000;                   // how often "bad_init" nodes fail
};

void register_types(ExecutionManager& m, SimPlatform& plat, Probe& pr) {
  m.register_factory("ok", [&pr](const NodeConfig& c) {
    return std::make_unique<LambdaNode>([&pr, n = c.name](const StepContext& s) {
      if (!pr.first_release.count(n)) pr.first_release[n] = s.release_us;
      pr.log.push_back(n);
      return true;
    });
  });
  m.register_factory("bad_init", [](const NodeConfig&) {
    return std::make_unique<LambdaNode>([](const StepContext&) { return true; }, false);
  });
  m.register_factory("failing", [&pr](const NodeConfig& c) {
    return std::make_unique<LambdaNode>([&pr, n = c.name](const StepContext&) { pr.log.push_back(n); return false; });
  });
  m.register_factory("hog", [&plat](const NodeConfig& c) {
    return std::make_unique<LambdaNode>([&plat, p = c.period_us](const StepContext&) {
      plat.manual_clock().advance(p);
      return true;
    });
  });
}

struct Rig {
  SimPlatform plat;
  ExecutionManager m{plat, ExecutionManager::Mode::Tick};
  Probe pr;
  Rig() { register_types(m, plat, pr); }
  bool load(const char* text, std::string& err) {
    LaunchConfig c;
    return LaunchConfig::parse(text, c, err) && m.configure(c, err) && m.start(err, false);
  }
  void run(TimeUs until) { drive(plat, *m.tick_scheduler(), until); }
};

}  // namespace

// First-release times honour delay_ms and the after= chain.
TEST(manager_launch_sequence_offsets_and_dependencies) {
  Rig r;
  std::string err;
  CHECK(r.load("node a type=ok period_ms=10\nnode b type=ok period_ms=10\nnode c type=ok period_ms=10\n"
               "start c after=b delay_ms=4\nstart a\nstart b after=a delay_ms=6\n", err));
  r.run(30 * kMs);
  CHECK_EQ(r.pr.first_release["a"], 0u);
  CHECK_EQ(r.pr.first_release["b"], 6 * kMs);   // after a (0) + 6
  CHECK_EQ(r.pr.first_release["c"], 10 * kMs);  // after b (6) + 4
  CHECK(r.m.state() == ManagerState::Running);
}

// A failed on_init blocks every dependent node from starting.
TEST(manager_skips_dependents_of_failed_init) {
  Rig r;
  std::string err;
  CHECK(r.load("node a type=bad_init period_ms=10\nnode b type=ok period_ms=10\nnode c type=ok period_ms=10\n"
               "start a\nstart b after=a\nstart c\n", err));
  r.run(30 * kMs);
  NodeStatus a, b, c;
  CHECK(r.m.node("a", a) && r.m.node("b", b) && r.m.node("c", c));
  CHECK(a.state == NodeState::Faulted);
  CHECK(b.state == NodeState::Stopped && b.stats.cycles == 0);
  CHECK(c.state == NodeState::Running && c.stats.cycles > 0);
}

// Configuration and lifecycle misuse is reported, not ignored.
TEST(manager_rejects_unknown_factory_and_double_start) {
  Rig r;
  std::string err;
  CHECK(!r.load("node a type=nope period_ms=10\nstart a\n", err));
  CHECK(err.find("nope") != std::string::npos);
  Rig r2;
  CHECK(r2.load("node a type=ok period_ms=10\nstart a\n", err));
  CHECK(!r2.m.start(err, false));
}

// Restart policy retries up to max_restarts, then latches safe state and fires the hook.
TEST(manager_restarts_failing_node_then_enters_safe_state) {
  Rig r;
  std::string err;
  int hook = 0;
  r.m.set_safe_state_hook([&] { ++hook; });
  CHECK(r.load("node f type=failing period_ms=10 max_restarts=2 fault_threshold=2\nstart f\n", err));
  r.run(400 * kMs);
  NodeStatus f;
  CHECK(r.m.node("f", f));
  CHECK_EQ(f.restarts, 2u);
  CHECK(r.m.state() == ManagerState::SafeState);
  CHECK(f.state == NodeState::Stopped);
  CHECK_EQ(hook, 1);
  // Once in safe state nothing runs any more.
  const auto cycles = f.stats.cycles;
  r.run(600 * kMs);
  r.m.node("f", f);
  CHECK_EQ(f.stats.cycles, cycles);
}

// on_fault=stop halts only the faulty node; the others keep running.
TEST(manager_stop_policy_isolates_starved_node) {
  Rig r;
  std::string err;
  CHECK(r.load("node hog type=hog priority=200 period_ms=10 on_fault=ignore\n"
               "node vic type=ok priority=10 period_ms=20 hb_timeout_ms=60 on_fault=stop\n"
               "start hog\nstart vic\n", err));
  r.run(300 * kMs);
  NodeStatus hog, vic;
  r.m.node("hog", hog);
  r.m.node("vic", vic);
  CHECK(vic.state == NodeState::Stopped);
  CHECK(hog.state == NodeState::Running);
  CHECK(r.m.state() == ManagerState::Running);
  bool saw_fault = false;
  for (auto& e : r.m.events_since(0)) saw_fault |= (e.kind == "fault" && e.node == "vic");
  CHECK(saw_fault);
}

// Operator restart/stop commands (the gRPC path) act on the right node.
TEST(manager_commands_restart_and_stop) {
  Rig r;
  std::string err;
  CHECK(r.load("node a type=ok period_ms=10\nstart a\n", err));
  r.run(50 * kMs);
  CHECK(r.m.restart_node("a"));
  NodeStatus a;
  r.m.node("a", a);
  CHECK_EQ(a.restarts, 1u);
  CHECK(r.m.stop_node("a"));
  r.m.node("a", a);
  CHECK(a.state == NodeState::Stopped);
  CHECK(!r.m.restart_node("missing"));
}

// events_since returns each event exactly once for a streaming client.
TEST(manager_event_cursor) {
  Rig r;
  std::string err;
  CHECK(r.load("node a type=ok period_ms=10\nstart a\n", err));
  auto all = r.m.events_since(0);
  CHECK(all.size() >= 2);
  CHECK(r.m.events_since(all.back().seq).empty());
}

namespace {
struct CountingHw : IHwWatchdog {
  int kicks = 0;
  void kick() override { ++kicks; }
};
}  // namespace

// HW watchdog is fed only while all heartbeats are fresh.
TEST(manager_kicks_hw_watchdog_only_while_healthy) {
  SimPlatform plat;
  CountingHw hw;
  ExecutionManager m(plat, ExecutionManager::Mode::Tick, &hw);
  Probe pr;
  register_types(m, plat, pr);
  LaunchConfig c;
  std::string err;
  CHECK(LaunchConfig::parse("node hog type=hog priority=200 period_ms=10 on_fault=ignore\n"
                            "node vic type=ok priority=10 period_ms=20 hb_timeout_ms=60 on_fault=ignore\n"
                            "start hog\nstart vic\n", c, err));
  CHECK(m.configure(c, err) && m.start(err, false));
  drive(plat, *m.tick_scheduler(), 50 * kMs);
  const int healthy_kicks = hw.kicks;
  CHECK(healthy_kicks > 0);
  drive(plat, *m.tick_scheduler(), 300 * kMs);  // victim starves -> unhealthy, kicks stop
  const int after = hw.kicks;
  drive(plat, *m.tick_scheduler(), 400 * kMs);
  CHECK_EQ(hw.kicks, after);
}
