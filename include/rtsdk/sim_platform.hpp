// Simulation platform: ManualClock + plain std::threads (no RT priorities).
// Lets the whole stack run deterministically in unit tests on any host.
#pragma once

#include <thread>

#include "rtsdk/osal.hpp"

namespace rtsdk {

class SimPlatform final : public IPlatform {
 public:
  IClock& clock() override { return clock_; }
  ManualClock& manual_clock() { return clock_; }
  bool realtime_priorities_active() const override { return false; }

  std::unique_ptr<IThread> spawn(const ThreadAttr&, std::function<void()> body) override {
    struct T final : IThread {
      std::thread t;
      void join() override {
        if (t.joinable()) t.join();
      }
      ~T() override { join(); }
    };
    auto t = std::make_unique<T>();
    t->t = std::thread(std::move(body));
    return t;
  }

 private:
  ManualClock clock_;
};

}  // namespace rtsdk
