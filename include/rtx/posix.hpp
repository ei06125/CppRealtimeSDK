// Linux execution adapters: one cooperative dispatcher or one worker per node.
// A runner owns one execution attempt; stop waits for active application
// callbacks to return.
#pragma once
#include <atomic>
#include <condition_variable>

#include "rtx/posix_sync.hpp"
#include "rtx/scheduler.hpp"
namespace rtx {
Time monotonic_now();
// Requests SCHED_FIFO for the calling thread. Errors are explicit (usually
// EPERM).
void set_fifo_priority(int priority);
class PosixRunner {
 public:
  explicit PosixRunner(Scheduler& scheduler,
                       Duration resolution = std::chrono::milliseconds(1));
  // Owns a watchdog supervisor thread; returns after callbacks and supervisor
  // exit. request_stop is sticky: construct a new runner for a new execution
  // session.
  void run();
  void request_stop() noexcept;

 private:
  Scheduler& scheduler_;
  Duration resolution_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> started_{false};
  RuntimeMutex wake_mutex_;
  detail::PosixConditionVariable wake_;
};
// One OS thread per node, plus an independent watchdog supervisor.
// FIFO mode reserves max(node priority)+1 for supervision. Node priorities must
// leave room below the OS FIFO maximum. Supervision is configured before
// launch. Failure to configure any worker aborts startup; no silent scheduling
// fallback.
class PosixTaskRunner {
 public:
  explicit PosixTaskRunner(Scheduler& scheduler, bool fifo = true,
                           Duration resolution = std::chrono::milliseconds(1));
  void run();
  void request_stop() noexcept;

 private:
  Scheduler& scheduler_;
  bool fifo_;
  Duration resolution_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> started_{false};
  RuntimeMutex wake_mutex_;
  detail::PosixConditionVariable wake_;
};
}  // namespace rtx
