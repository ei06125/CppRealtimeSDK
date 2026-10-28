// TickScheduler tests: priority order, periodicity, deadlines, starvation.
// A ManualClock lets the test advance time exactly, with no sleeping.
#include <string>
#include <vector>

#include "rt_test.hpp"
#include "test_support.hpp"

using namespace testutil;

namespace {

struct Fixture {
  SimPlatform plat;
  HeartbeatMonitor hb;
  Watchdog wd{plat.clock(), hb};
  TickScheduler ts{plat, wd, 1 * kMs};
  std::vector<std::unique_ptr<NodeRunner>> runners;
  std::vector<LambdaNode*> nodes;

  NodeRunner& add(const char* name, Priority prio, TimeUs period,
                  LambdaNode::Step step, TimeUs first = 0,
                  TimeUs hb_timeout = 0) {
    NodeConfig c;
    c.name = name;
    c.priority = prio;
    c.period_us = period;
    c.heartbeat_timeout_us = hb_timeout ? hb_timeout : 3 * period;
    hb.add(c.heartbeat_timeout_us);
    auto n = std::make_unique<LambdaNode>(std::move(step));
    nodes.push_back(n.get());
    runners.push_back(
        std::make_unique<NodeRunner>(runners.size(), c, std::move(n)));
    NodeRunner& r = *runners.back();
    r.init();
    r.start();
    hb.enable(r.id(), first);
    ts.add(r, first);
    return r;
  }
};

}  // namespace

// Among simultaneously pending jobs the highest priority runs first.
TEST(scheduler_dispatches_highest_priority_first) {
  Fixture f;
  std::vector<std::string> order;
  auto rec = [&](const char* n) {
    return [&order, n](const StepContext&) {
      order.push_back(n);
      return true;
    };
  };
  f.add("low", 10, 10 * kMs, rec("low"));
  f.add("mid", 50, 10 * kMs, rec("mid"));
  f.add("high", 200, 10 * kMs, rec("high"));
  f.ts.tick();
  CHECK_EQ(order.size(), 3u);
  CHECK(order[0] == "high" && order[1] == "mid" && order[2] == "low");
}

// Ties are broken by earliest release time.
TEST(scheduler_equal_priority_is_fifo_by_release) {
  Fixture f;
  std::vector<std::string> order;
  auto rec = [&](const char* n) {
    return [&order, n](const StepContext&) {
      order.push_back(n);
      return true;
    };
  };
  f.add("late", 100, 10 * kMs, rec("late"), 2 * kMs);
  f.add("early", 100, 10 * kMs, rec("early"), 1 * kMs);
  drive(f.plat, f.ts, 1 * kMs);  // nothing due before 1ms
  CHECK(order.empty());
  f.plat.manual_clock().set(5 * kMs);  // both pending at once
  f.ts.tick();
  CHECK(order.size() == 2 && order[0] == "early" && order[1] == "late");
}

// Number of jobs over a window equals window / period.
TEST(scheduler_periodic_release_count) {
  Fixture f;
  auto& r = f.add("p", 100, 10 * kMs, [](const StepContext&) { return true; });
  drive(f.plat, f.ts, 100 * kMs);  // releases at 0,10,...,90
  CHECK_EQ(r.stats().cycles, 10u);
  CHECK_EQ(r.stats().deadline_misses, 0u);
  CHECK_EQ(f.ts.tick_count(), 100u);
}

// Release instants stay on the period grid regardless of execution time.
TEST(scheduler_release_times_are_drift_free) {
  Fixture f;
  std::vector<TimeUs> rel;
  f.add(
      "p", 100, 10 * kMs,
      [&](const StepContext& c) {
        rel.push_back(c.release_us);
        return true;
      },
      3 * kMs);
  drive(f.plat, f.ts, 50 * kMs);
  CHECK_EQ(rel.size(), 5u);
  for (std::size_t i = 0; i < rel.size(); ++i)
    CHECK_EQ(rel[i], 3 * kMs + i * 10 * kMs);
}

// Misses are counted every time but reported once at fault_threshold.
TEST(scheduler_deadline_miss_is_counted_and_reported_at_threshold) {
  Fixture f;
  int reported = 0;
  f.wd.set_handler([&](const Fault& ft) {
    reported += (ft.kind == FaultKind::DeadlineMiss);
  });
  auto& r = f.add("slow", 100, 10 * kMs, [&](const StepContext&) {
    f.plat.manual_clock().advance(12 * kMs);  // exceeds 10ms deadline
    return true;
  });
  drive(f.plat, f.ts, 100 * kMs);
  CHECK(r.stats().deadline_misses >= 3);
  CHECK_EQ(reported, 1);  // fault_threshold=3, latched once per episode
  CHECK(r.stats().skipped_releases > 0);
}

// A long low-priority job shows up as latency of the higher-priority one.
TEST(scheduler_nonpreemptive_latency_is_measured) {
  Fixture f;
  // low starts at t=0 and runs 5ms; high is released at t=2ms and must wait.
  f.add("low", 10, 50 * kMs, [&](const StepContext&) {
    f.plat.manual_clock().advance(5 * kMs);
    return true;
  });
  auto& hi = f.add(
      "high", 200, 50 * kMs, [](const StepContext&) { return true; }, 2 * kMs);
  drive(f.plat, f.ts, 10 * kMs);
  CHECK_EQ(hi.stats().max_latency_us, 3 * kMs);
}

// A job released during a tick preempts at the next job boundary.
TEST(scheduler_higher_priority_released_midtick_runs_before_lower_pending) {
  Fixture f;
  std::vector<std::string> order;
  // blocker occupies 3ms; meanwhile low (t=1) and high (t=2) both become ready.
  f.add("blocker", 255, 100 * kMs, [&](const StepContext&) {
    order.push_back("blocker");
    f.plat.manual_clock().advance(3 * kMs);
    return true;
  });
  f.add(
      "low", 10, 100 * kMs,
      [&](const StepContext&) {
        order.push_back("low");
        return true;
      },
      1 * kMs);
  f.add(
      "high", 100, 100 * kMs,
      [&](const StepContext&) {
        order.push_back("high");
        return true;
      },
      2 * kMs);
  drive(f.plat, f.ts, 10 * kMs);
  CHECK(order.size() == 3 && order[0] == "blocker" && order[1] == "high" &&
        order[2] == "low");
}

// A starved node stops beating and the watchdog flags it.
TEST(scheduler_starvation_triggers_heartbeat_timeout) {
  Fixture f;
  std::vector<Fault> faults;
  f.wd.set_handler([&](const Fault& ft) { faults.push_back(ft); });
  // hog consumes the whole 10ms period at top priority -> victim never runs.
  f.add("hog", 200, 10 * kMs, [&](const StepContext&) {
    f.plat.manual_clock().advance(10 * kMs);
    return true;
  });
  f.add(
      "victim", 10, 20 * kMs, [](const StepContext&) { return true; }, 0,
      60 * kMs);
  drive(f.plat, f.ts, 200 * kMs);
  bool hb_fault = false;
  for (auto& ft : faults)
    hb_fault |= (ft.node == 1 && ft.kind == FaultKind::HeartbeatTimeout);
  CHECK(hb_fault);
}

// A node that is not Running is never dispatched.
TEST(scheduler_failed_node_is_not_released) {
  Fixture f;
  auto& r = f.add("p", 100, 10 * kMs, [](const StepContext&) { return true; });
  r.set_state(NodeState::Stopped);
  drive(f.plat, f.ts, 50 * kMs);
  CHECK_EQ(r.stats().cycles, 0u);
}
