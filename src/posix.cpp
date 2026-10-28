// Checked Linux monotonic time, FIFO policy requests and cooperative execution.
// Idle waits are interruptible; the separate supervisor observes callbacks
// without cancelling them.
#include "rtx/posix.hpp"

#include <pthread.h>
#include <time.h>

#include <cerrno>
#include <condition_variable>
#include <stdexcept>
#include <system_error>
#include <thread>

#include "time_math.hpp"
namespace rtx {
Time monotonic_now() {
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    throw std::system_error(errno, std::generic_category(), "clock_gettime");
  if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000 ||
      ts.tv_sec > (Time::max().count() - ts.tv_nsec) / 1000000000)
    throw std::overflow_error("monotonic clock outside nanosecond range");
  return Time(ts.tv_sec * 1000000000 + ts.tv_nsec);
}
void set_fifo_priority(int priority) {
  if (priority < sched_get_priority_min(SCHED_FIFO) ||
      priority > sched_get_priority_max(SCHED_FIFO))
    throw std::invalid_argument("FIFO priority out of range");
  sched_param param{};
  param.sched_priority = priority;
  int result = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
  if (result)
    throw std::system_error(result, std::generic_category(),
                            "pthread_setschedparam");
}
PosixRunner::PosixRunner(Scheduler& scheduler, Duration resolution)
    : scheduler_(scheduler), resolution_(resolution) {
  if (resolution <= Duration::zero())
    throw std::invalid_argument("resolution must be positive");
}
void PosixRunner::request_stop() noexcept {
  {
    std::lock_guard lock(wake_mutex_);
    stopping_.store(true);
  }
  wake_.notify_all();
}
void PosixRunner::run() {
  if (started_.exchange(true))
    throw std::logic_error("runner can only be started once");
  auto next = monotonic_now();
  scheduler_.start(next);
  std::exception_ptr supervisor_failure;
  std::jthread supervisor;
  try {
    supervisor = std::jthread([&](std::stop_token token) {
      try {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        while (!token.stop_requested() && !stopping_.load()) {
          scheduler_.inspect_watchdogs(monotonic_now());
          wake.wait_for(lock, token, resolution_, [] { return false; });
        }
      } catch (...) {
        supervisor_failure = std::current_exception();
        request_stop();
      }
    });
    while (!stopping_.load()) {
      auto now = monotonic_now();
      scheduler_.step(now, monotonic_now);
      now = monotonic_now();
      next = detail::next_release(next, now, resolution_);
      // Keep the absolute monotonic release phase, but make the idle wait
      // interruptible. A stop must not wait for a potentially long resolution.
      std::unique_lock lock(wake_mutex_);
      while (!stopping_.load()) {
        const auto remaining = next - monotonic_now();
        if (remaining <= Duration::zero()) break;
        wake_.wait_for(lock, remaining, [&] { return stopping_.load(); });
      }
    }
  } catch (...) {
    supervisor.request_stop();
    if (supervisor.joinable()) supervisor.join();
    scheduler_.stop();
    throw;
  }
  supervisor.request_stop();
  supervisor.join();
  scheduler_.stop();
  if (supervisor_failure) std::rethrow_exception(supervisor_failure);
}
}  // namespace rtx
