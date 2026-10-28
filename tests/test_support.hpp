// Shared fixtures for tests: helpers that wire a ManualClock, SimPlatform and
// recording fault handler together so each test stays short.
#pragma once

#include <functional>

#include "rtsdk/executor.hpp"
#include "rtsdk/sim_platform.hpp"

namespace testutil {

using namespace rtsdk;

// Node whose step body is a lambda; counts init/shutdown calls.
class LambdaNode final : public Node {
 public:
  using Step = std::function<bool(const StepContext&)>;
  explicit LambdaNode(Step s, bool init_ok = true)
      : step_(std::move(s)), init_ok_(init_ok) {}
  bool on_init() override {
    ++inits;
    return init_ok_;
  }
  bool on_step(const StepContext& c) override { return step_(c); }
  void on_shutdown() override { ++shutdowns; }
  int inits = 0, shutdowns = 0;

 private:
  Step step_;
  bool init_ok_;
};

// Advances simulated time tick by tick, calling TickScheduler::tick() each
// time.
inline void drive(SimPlatform& p, TickScheduler& ts, TimeUs until_us) {
  ManualClock& c = p.manual_clock();
  while (c.now_us() < until_us) {
    const TimeUs t0 = c.now_us();
    ts.tick();
    c.sleep_until(t0 + ts.tick_us());
  }
}

}  // namespace testutil
