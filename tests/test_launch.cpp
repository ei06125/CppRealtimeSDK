// Launch-config parser and dependency resolver tests (pure logic, no threads).
#include "rt_test.hpp"
#include "rtsdk/launch.hpp"

using namespace rtsdk;

static const char* kGood = R"(
# demo
node camera  type=sensor  priority=200 period_ms=33 on_fault=restart
node fusion  type=compute priority=150 period_ms=20 deadline_ms=15 hb_timeout_ms=100
node planner type=compute priority=100 period_ms=50 on_fault=safe max_restarts=1
start planner after=fusion delay_ms=5
start camera
start fusion after=camera
)";

// Every node/start key is parsed with correct unit scaling.
TEST(launch_parse_fields) {
  LaunchConfig c;
  std::string err;
  CHECK(LaunchConfig::parse(kGood, c, err));
  CHECK_EQ(c.nodes.size(), 3u);
  CHECK_EQ(c.steps.size(), 3u);
  CHECK(c.nodes[0].config.name == "camera" && c.nodes[0].type == "sensor");
  CHECK_EQ(c.nodes[0].config.priority, 200u);
  CHECK_EQ(c.nodes[0].config.period_us, 33000u);
  CHECK_EQ(c.nodes[1].config.deadline_us, 15000u);
  CHECK_EQ(c.nodes[1].config.heartbeat_timeout_us, 100000u);
  CHECK(c.nodes[2].config.on_fault == FaultAction::SafeState);
  CHECK_EQ(c.nodes[2].config.max_restarts, 1u);
  CHECK_EQ(c.steps[0].delay_us, 5000u);
  CHECK(c.steps[0].after.size() == 1 && c.steps[0].after[0] == "fusion");
}

// after= dependencies yield a valid topological start order.
TEST(launch_resolves_dependency_order) {
  LaunchConfig c;
  std::string err;
  CHECK(LaunchConfig::parse(kGood, c, err));
  std::vector<std::size_t> order;
  CHECK(c.resolve_order(order, err));
  // steps: 0=planner(after fusion) 1=camera 2=fusion(after camera) -> camera, fusion, planner
  CHECK(order.size() == 3 && order[0] == 1 && order[1] == 2 && order[2] == 0);
}

// Mutually dependent steps are rejected instead of deadlocking at start.
TEST(launch_detects_cycle) {
  LaunchConfig c;
  std::string err;
  CHECK(LaunchConfig::parse("node a type=t period_ms=1\nnode b type=t period_ms=1\n"
                            "start a after=b\nstart b after=a\n", c, err));
  std::vector<std::size_t> order;
  CHECK(!c.resolve_order(order, err));
  CHECK(err.find("cycle") != std::string::npos);
}

// Malformed lines, unknown keys and duplicates report errors with line numbers.
TEST(launch_rejects_bad_input) {
  LaunchConfig c;
  std::string err;
  CHECK(!LaunchConfig::parse("node a priority=1 period_ms=1\n", c, err));            // no type
  CHECK(!LaunchConfig::parse("node a type=t priority=300 period_ms=1\n", c, err));   // prio range
  CHECK(!LaunchConfig::parse("node a type=t period_ms=0\n", c, err));                // zero period
  CHECK(!LaunchConfig::parse("node a type=t period_ms=x\n", c, err));                // not a number
  CHECK(!LaunchConfig::parse("node a type=t bogus=1\n", c, err));                    // unknown key
  CHECK(!LaunchConfig::parse("frob a\n", c, err));                                   // unknown directive
  CHECK(!LaunchConfig::parse("node a type=t\nnode a type=t\n", c, err));             // duplicate
  CHECK(err.find("line 2") != std::string::npos);
  CHECK(LaunchConfig::parse("node a type=t\nstart b\n", c, err));
  std::vector<std::size_t> order;
  CHECK(!c.resolve_order(order, err));                                               // unknown node
}
