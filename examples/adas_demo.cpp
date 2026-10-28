// ADAS pipeline demo: camera -> fusion -> planner, connected by shared-memory
// primitives, launched from a text config, supervised by the watchdog.
#include <chrono>
#include <cstdio>
#include <thread>

#include "rtsdk/comms.hpp"
#include "rtsdk/manager.hpp"
#include "rtsdk/posix_platform.hpp"

using namespace rtsdk;

namespace {

struct Frame { std::uint64_t id; double obstacle_m; };
struct Plan { std::uint64_t frame_id; double brake; };

struct Bus {
  HeapRegion frames{SpscRing::required_bytes(16, sizeof(Frame))};
  HeapRegion plans{Mailbox<Plan>::required_bytes()};
  SpscRing frame_ring = SpscRing::create(frames.data(), 16, sizeof(Frame));
  Mailbox<Plan> plan_box = Mailbox<Plan>::create(plans.data());
};

struct Camera final : Node {
  explicit Camera(Bus& b) : bus(b) {}
  bool on_step(const StepContext& c) override {
    Frame f{c.cycle, 50.0 - static_cast<double>(c.cycle % 40)};
    bus.frame_ring.try_push(&f, sizeof(f));
    return true;
  }
  Bus& bus;
};

struct Fusion final : Node {
  explicit Fusion(Bus& b) : bus(b) {}
  bool on_step(const StepContext&) override {
    Frame f;
    std::uint32_t len;
    while (bus.frame_ring.try_pop(&f, &len)) last = f;
    return true;
  }
  Bus& bus;
  Frame last{0, 100};
};

struct Planner final : Node {
  explicit Planner(Bus& b) : bus(b) {}
  bool on_step(const StepContext& c) override {
    // Trivial policy: brake harder the closer the obstacle.
    Plan p{c.cycle, 0.0};
    bus.plan_box.write(p);
    return true;
  }
  Bus& bus;
};

const char* kConfig = R"(
node camera  type=camera  priority=200 period_ms=10 on_fault=restart
node fusion  type=fusion  priority=150 period_ms=20 on_fault=restart
node planner type=planner priority=100 period_ms=50 on_fault=safe
start camera
start fusion  after=camera delay_ms=5
start planner after=fusion delay_ms=10
)";

}  // namespace

int main() {
  PosixPlatform plat;
  Bus bus;
  ExecutionManager mgr(plat, ExecutionManager::Mode::Threads);
  mgr.register_factory("camera", [&](const NodeConfig&) { return std::make_unique<Camera>(bus); });
  mgr.register_factory("fusion", [&](const NodeConfig&) { return std::make_unique<Fusion>(bus); });
  mgr.register_factory("planner", [&](const NodeConfig&) { return std::make_unique<Planner>(bus); });
  mgr.set_safe_state_hook([] { std::puts("!! SAFE STATE: commanding minimal-risk manoeuvre"); });

  LaunchConfig cfg;
  std::string err;
  if (!LaunchConfig::parse(kConfig, cfg, err) || !mgr.configure(cfg, err) || !mgr.start(err)) {
    std::fprintf(stderr, "launch failed: %s\n", err.c_str());
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(1));
  for (const auto& n : mgr.nodes())
    std::printf("%-8s %-10s prio=%3u cycles=%4llu misses=%llu max_exec=%lluus\n", n.name.c_str(), to_string(n.state),
                n.priority, static_cast<unsigned long long>(n.stats.cycles),
                static_cast<unsigned long long>(n.stats.deadline_misses),
                static_cast<unsigned long long>(n.stats.max_exec_us));
  Plan p;
  if (bus.plan_box.read(p)) std::printf("latest plan: frame=%llu\n", static_cast<unsigned long long>(p.frame_id));
  mgr.stop();
  return 0;
}
