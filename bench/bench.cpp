// Performance benchmarks. `rtsdk_bench` prints numbers; `rtsdk_bench --check`
// additionally fails (exit 1) when a generous regression budget is exceeded.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "rtsdk/comms.hpp"
#include "rtsdk/executor.hpp"
#include "rtsdk/posix_platform.hpp"
#include "rtsdk/sim_platform.hpp"

using namespace rtsdk;
using Clock = std::chrono::steady_clock;

namespace {

int g_failed = 0;
bool g_check = false;

void report(const char* name, double value, const char* unit, double budget,
            bool lower_is_better = true) {
  const bool ok =
      !g_check || (lower_is_better ? value <= budget : value >= budget);
  std::printf("%-44s %12.1f %-8s (budget %s %.1f) %s\n", name, value, unit,
              lower_is_better ? "<=" : ">=", budget,
              ok ? "" : "** OVER BUDGET **");
  g_failed += !ok;
}

struct NopNode final : Node {
  bool on_step(const StepContext&) override { return true; }
};

// Cost of the scheduling machinery itself: release + pick + execute_job.
void bench_dispatch(std::size_t n_nodes) {
  SimPlatform plat;
  HeartbeatMonitor hb;
  Watchdog wd(plat.clock(), hb);
  TickScheduler ts(plat, wd, kMs);
  std::vector<std::unique_ptr<NodeRunner>> rs;
  for (std::size_t i = 0; i < n_nodes; ++i) {
    NodeConfig c;
    c.name = "n" + std::to_string(i);
    c.priority = static_cast<Priority>(i);
    c.period_us = kMs;  // every node is released each tick
    c.heartbeat_timeout_us = kSec;
    hb.add(c.heartbeat_timeout_us);
    rs.push_back(
        std::make_unique<NodeRunner>(i, c, std::make_unique<NopNode>()));
    rs.back()->init();
    rs.back()->start();
    hb.enable(i, 0);
    ts.add(*rs.back(), 0);
  }
  constexpr int kTicks = 20000;
  const auto t0 = Clock::now();
  for (int i = 0; i < kTicks; ++i) {
    ts.tick();
    plat.manual_clock().advance(kMs);
  }
  const double ns =
      std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
  char name[64];
  std::snprintf(name, sizeof(name), "dispatch cost per job (%zu nodes)",
                n_nodes);
  report(name,
         ns / (static_cast<double>(kTicks) * static_cast<double>(n_nodes)),
         "ns/job", 5000);
}

void bench_ring() {
  constexpr std::uint32_t kN = 2000000;
  HeapRegion mem(SpscRing::required_bytes(1024, 64));
  SpscRing ring = SpscRing::create(mem.data(), 1024, 64);
  const auto t0 = Clock::now();
  std::thread prod([&] {
    unsigned char msg[64] = {};
    for (std::uint32_t i = 0; i < kN;) {
      std::memcpy(msg, &i, sizeof(i));
      if (ring.try_push(msg, sizeof(msg))) ++i;
    }
  });
  unsigned char out[64];
  std::uint32_t got = 0, len;
  while (got < kN)
    if (ring.try_pop(out, &len)) ++got;
  prod.join();
  const double s = std::chrono::duration<double>(Clock::now() - t0).count();
  report("shm ring throughput (64B msgs, 2 threads)", kN / s / 1e6, "Mmsg/s",
         0.2, false);
}

void bench_mailbox() {
  struct P {
    double v[6];
  };
  HeapRegion mem(Mailbox<P>::required_bytes());
  auto mb = Mailbox<P>::create(mem.data());
  constexpr int kN = 5000000;
  P p{};
  auto t0 = Clock::now();
  for (int i = 0; i < kN; ++i) {
    p.v[0] = i;
    mb.write(p);
  }
  report(
      "mailbox write (48B)",
      std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / kN,
      "ns", 500);
  t0 = Clock::now();
  double acc = 0;
  for (int i = 0; i < kN; ++i) {
    mb.read(p);
    acc += p.v[0];
  }
  report(
      "mailbox read  (48B)",
      std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / kN,
      "ns", 500);
  if (acc < 0) std::puts("");
}

// Real-time behaviour: release->start latency of a 1ms periodic node while a
// lower-priority node burns CPU. Budget is generous because without RT
// privileges the kernel schedules everything SCHED_OTHER.
void bench_latency() {
  PosixPlatform plat;
  HeartbeatMonitor hb;
  Watchdog wd(plat.clock(), hb);
  ThreadExecutor ex(plat, wd);
  std::vector<TimeUs> lat;
  lat.reserve(4000);
  struct Probe final : Node {
    std::vector<TimeUs>& l;
    explicit Probe(std::vector<TimeUs>& v) : l(v) {}
    bool on_step(const StepContext& c) override {
      if (l.size() < l.capacity()) l.push_back(c.now_us - c.release_us);
      return true;
    }
  };
  struct Burn final : Node {
    IClock& clk;
    explicit Burn(IClock& c) : clk(c) {}
    bool on_step(const StepContext&) override {
      const TimeUs e = clk.now_us() + 2 * kMs;
      while (clk.now_us() < e) {
      }
      return true;
    }
  };
  NodeConfig hc;
  hc.name = "probe";
  hc.priority = 250;
  hc.period_us = kMs;
  hc.heartbeat_timeout_us = kSec;
  NodeConfig bc;
  bc.name = "burn";
  bc.priority = 10;
  bc.period_us = 2 * kMs;
  bc.heartbeat_timeout_us = kSec;
  hb.add(hc.heartbeat_timeout_us);
  hb.add(bc.heartbeat_timeout_us);
  NodeRunner hr(0, hc, std::make_unique<Probe>(lat)),
      br(1, bc, std::make_unique<Burn>(plat.clock()));
  hr.init();
  hr.start();
  br.init();
  br.start();
  const TimeUs t0 = plat.clock().now_us() + 10 * kMs;
  hb.enable(0, t0);
  hb.enable(1, t0);
  ex.add(hr, t0);
  ex.add(br, t0);
  ex.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(4200));
  ex.stop();
  std::sort(lat.begin(), lat.end());
  if (lat.empty()) return;
  auto pct = [&](double p) {
    return static_cast<double>(
        lat[static_cast<std::size_t>(p * (lat.size() - 1))]);
  };
  std::printf("  (rt priorities %s, %zu samples)\n",
              plat.realtime_priorities_active() ? "ACTIVE" : "unavailable",
              lat.size());
  const bool rt = plat.realtime_priorities_active();
  report("start latency p50", pct(0.50), "us", rt ? 200 : 5000);
  report("start latency p99", pct(0.99), "us", rt ? 500 : 20000);
  report("start latency max", pct(1.0), "us", rt ? 2000 : 100000);
}

}  // namespace

int main(int argc, char** argv) {
  g_check = argc > 1 && std::strcmp(argv[1], "--check") == 0;
  bench_dispatch(4);
  bench_dispatch(32);
  bench_ring();
  bench_mailbox();
  bench_latency();
  std::printf("%s\n", g_failed ? "PERF CHECK FAILED" : "perf ok");
  return g_failed ? 1 : 0;
}
