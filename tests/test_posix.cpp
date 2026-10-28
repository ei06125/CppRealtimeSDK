// Real-clock tests on the POSIX backend (timing assertions use wide margins so
// they stay stable on loaded CI machines).
#include <algorithm>
#include <atomic>
#include <thread>

#include "rt_test.hpp"
#include "rtsdk/manager.hpp"
#include "rtsdk/posix_platform.hpp"
#include "test_support.hpp"

using namespace testutil;

TEST(posix_clock_is_monotonic_and_sleep_until_is_absolute) {
  PosixPlatform p;
  const TimeUs t0 = p.clock().now_us();
  p.clock().sleep_until(t0 + 20 * kMs);
  const TimeUs t1 = p.clock().now_us();
  CHECK(t1 >= t0 + 20 * kMs);
  CHECK(t1 < t0 + 200 * kMs);
}

TEST(posix_priority_mapping_is_monotonic) {
  int prev = 0;
  for (int p = 0; p <= 255; ++p) {
    const int m = PosixPlatform::map_priority(static_cast<Priority>(p));
    CHECK(m >= prev);
    prev = m;
  }
  CHECK(PosixPlatform::map_priority(0) >= 1);
  CHECK(PosixPlatform::map_priority(255) <= 99);
}

TEST(posix_thread_executor_runs_periodic_nodes_and_stops) {
  PosixPlatform plat;
  ExecutionManager m(plat, ExecutionManager::Mode::Threads);
  std::atomic<int> a{0}, b{0};
  m.register_factory("a", [&](const NodeConfig&) {
    return std::make_unique<LambdaNode>([&](const StepContext&) { ++a; return true; });
  });
  m.register_factory("b", [&](const NodeConfig&) {
    return std::make_unique<LambdaNode>([&](const StepContext&) { ++b; return true; });
  });
  LaunchConfig c;
  std::string err;
  CHECK(LaunchConfig::parse("node a type=a priority=200 period_ms=5\nnode b type=b priority=50 period_ms=10\n"
                            "start a\nstart b after=a\n", c, err));
  CHECK(m.configure(c, err) && m.start(err));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  m.stop();
  CHECK(a.load() >= 30 && a.load() <= 62);  // ~60 expected
  CHECK(b.load() >= 15 && b.load() <= 31);  // ~30 expected
  const int frozen = a.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_EQ(a.load(), frozen);
  CHECK(m.state() == ManagerState::Stopped);
}

TEST(posix_hung_node_escalates_to_safe_state) {
  PosixPlatform plat;
  ExecutionManager m(plat, ExecutionManager::Mode::Threads);
  std::atomic<bool> release{false}, hang{false};
  std::atomic<int> hooks{0};
  m.set_safe_state_hook([&] { ++hooks; });
  m.register_factory("hang", [&](const NodeConfig&) {
    return std::make_unique<LambdaNode>([&](const StepContext& s) {
      if (s.cycle == 3) {
        hang = true;
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return true;
    });
  });
  LaunchConfig c;
  std::string err;
  CHECK(LaunchConfig::parse("node h type=hang period_ms=5 hb_timeout_ms=30 on_fault=restart\nstart h\n", c, err));
  CHECK(m.configure(c, err) && m.start(err));
  for (int i = 0; i < 200 && m.state() == ManagerState::Running; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(hang.load());
  CHECK(m.state() == ManagerState::SafeState);
  CHECK_EQ(hooks.load(), 1);
  release = true;  // let the node thread finish so stop() can join
  m.stop();
}

TEST(posix_high_priority_node_has_bounded_latency_under_load) {
  // Informational on machines without RT privileges: only asserts a very
  // generous bound; the bench reports the real numbers.
  PosixPlatform plat;
  ExecutionManager m(plat, ExecutionManager::Mode::Threads);
  m.register_factory("hi", [](const NodeConfig&) {
    return std::make_unique<LambdaNode>([](const StepContext&) { return true; });
  });
  m.register_factory("busy", [&](const NodeConfig&) {
    return std::make_unique<LambdaNode>([&](const StepContext&) {
      const TimeUs end = plat.clock().now_us() + 3 * kMs;
      while (plat.clock().now_us() < end) {
      }
      return true;
    });
  });
  LaunchConfig c;
  std::string err;
  CHECK(LaunchConfig::parse("node hi type=hi priority=250 period_ms=5\nnode busy type=busy priority=10 period_ms=5\n"
                            "start hi\nstart busy\n", c, err));
  CHECK(m.configure(c, err) && m.start(err));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  m.stop();
  NodeStatus hi;
  CHECK(m.node("hi", hi));
  CHECK(hi.stats.cycles > 20);
  CHECK(hi.stats.max_latency_us < 100 * kMs);
  std::printf("    info: rt priorities %s, hi max latency %llu us\n",
              plat.realtime_priorities_active() ? "active" : "unavailable",
              static_cast<unsigned long long>(hi.stats.max_latency_us));
}
