// Executors decide *how* released jobs reach a CPU.
//  - TickScheduler: single-threaded, tick-driven, fixed-priority dispatch with
//    run-to-completion semantics. Deterministic, needs no OS threads: the
//    portable choice for bare-metal/RTOS single-task use and for simulation.
//  - ThreadExecutor: one OS thread per node at the node's RT priority, so the
//    OS kernel provides true preemption (POSIX SCHED_FIFO, FreeRTOS tasks).
#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "rtsdk/osal.hpp"
#include "rtsdk/runner.hpp"

namespace rtsdk {

class IExecutor {
 public:
  virtual ~IExecutor() = default;
  // Must be called before start(). first_release_us is the first nominal release.
  virtual void add(NodeRunner& r, TimeUs first_release_us) = 0;
  virtual void start() = 0;
  virtual void stop() = 0;
};

class TickScheduler final : public IExecutor {
 public:
  TickScheduler(IPlatform& platform, Watchdog& wd, TimeUs tick_us);
  ~TickScheduler() override { stop(); }

  void add(NodeRunner& r, TimeUs first_release_us) override;
  void start() override;  // runs tick() from a background thread at tick_us cadence
  void stop() override;

  // Release due jobs and dispatch by priority until the tick budget is used up
  // or nothing is ready. Then runs the watchdog. Call directly for manual driving.
  void tick();

  std::uint64_t tick_count() const { return ticks_.load(std::memory_order_relaxed); }
  TimeUs tick_us() const { return tick_us_; }

 private:
  struct Slot {
    NodeRunner* runner;
    TimeUs next_release;
    TimeUs pending_release = 0;
    std::uint64_t cycle = 0;
    bool pending = false;
  };
  void release_due(TimeUs now);
  Slot* pick();

  IPlatform& platform_;
  Watchdog& wd_;
  TimeUs tick_us_;
  std::vector<Slot> slots_;
  std::atomic<std::uint64_t> ticks_{0};
  std::atomic<bool> stop_{false};
  std::unique_ptr<IThread> thread_;
};

class ThreadExecutor final : public IExecutor {
 public:
  ThreadExecutor(IPlatform& platform, Watchdog& wd, TimeUs supervisor_period_us = 5 * kMs,
                 Priority supervisor_priority = 255);
  ~ThreadExecutor() override { stop(); }

  void add(NodeRunner& r, TimeUs first_release_us) override;
  void start() override;
  void stop() override;

 private:
  void node_loop(NodeRunner& r, TimeUs first_release);
  void supervisor_loop();

  IPlatform& platform_;
  Watchdog& wd_;
  TimeUs supervisor_period_;
  Priority supervisor_priority_;
  std::vector<std::pair<NodeRunner*, TimeUs>> nodes_;
  std::vector<std::unique_ptr<IThread>> threads_;
  std::atomic<bool> stop_{false};
};

}  // namespace rtsdk
