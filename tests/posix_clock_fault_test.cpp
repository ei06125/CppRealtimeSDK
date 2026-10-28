// Inject native clock and scheduling-policy failures without production fault
// hooks. Check error provenance, launch suppression and cleanup of workers
// waiting at startup gates.
#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <thread>

#include "rtx/posix.hpp"
using namespace std::chrono_literals;
namespace {
enum class Mode {
  real,
  error,
  negative_seconds,
  negative_nanoseconds,
  invalid_nanoseconds,
  overflow,
  maximum,
  owner_runtime,
  background
};
std::atomic<Mode> mode{Mode::real};
std::atomic<int> policy_failure_priority = 0;
const auto owner = std::this_thread::get_id();
thread_local unsigned calls = 0;
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}  // namespace
extern "C" int __real_clock_gettime(clockid_t, timespec*);
extern "C" int __wrap_clock_gettime(clockid_t clock, timespec* value) {
  if (clock != CLOCK_MONOTONIC) return __real_clock_gettime(clock, value);
  const auto selected = mode.load();
  if (selected == Mode::error ||
      (selected == Mode::owner_runtime && std::this_thread::get_id() == owner &&
       ++calls == 2) ||
      (selected == Mode::background && std::this_thread::get_id() != owner)) {
    errno = EIO;
    return -1;
  }
  switch (selected) {
    case Mode::negative_seconds:
      *value = {-1, 0};
      return 0;
    case Mode::negative_nanoseconds:
      *value = {0, -1};
      return 0;
    case Mode::invalid_nanoseconds:
      *value = {0, 1000000000};
      return 0;
    case Mode::overflow:
      *value = {std::numeric_limits<time_t>::max(), 999999999};
      return 0;
    case Mode::maximum:
      *value = {rtx::Time::max().count() / 1000000000,
                rtx::Time::max().count() % 1000000000};
      return 0;
    default:
      return __real_clock_gettime(clock, value);
  }
}
extern "C" int __real_pthread_setschedparam(pthread_t, int, const sched_param*);
extern "C" int __wrap_pthread_setschedparam(pthread_t thread, int policy,
                                            const sched_param* parameter) {
  const auto failure = policy_failure_priority.load();
  if (failure) return parameter->sched_priority == failure ? EPERM : 0;
  return __real_pthread_setschedparam(thread, policy, parameter);
}
int main() {
  try {
    mode = Mode::error;
    bool rejected = false;
    try {
      rtx::monotonic_now();
    } catch (const std::system_error& error) {
      rejected = error.code().value() == EIO;
    }
    require(rejected, "clock errno was not preserved");
    for (auto invalid : {Mode::negative_seconds, Mode::negative_nanoseconds,
                         Mode::invalid_nanoseconds, Mode::overflow}) {
      mode = invalid;
      rejected = false;
      try {
        rtx::monotonic_now();
      } catch (const std::overflow_error&) {
        rejected = true;
      }
      require(rejected, "invalid clock value accepted");
    }
    mode = Mode::maximum;
    require(rtx::monotonic_now() == rtx::Time::max(),
            "maximum representable clock rejected");
    // Failure after scheduler.start must stop health and join the supervisor.
    unsigned invoked = 0;
    rtx::Scheduler cooperative(
        {{"node", 1, 1ms, 1s, 0ns, {}, [&](auto&) { ++invoked; }}});
    rtx::PosixRunner runner(cooperative);
    calls = 0;
    mode = Mode::owner_runtime;
    rejected = false;
    try {
      runner.run();
    } catch (const std::system_error& error) {
      rejected = error.code().value() == EIO;
    }
    mode = Mode::real;
    require(rejected && invoked == 0 &&
                cooperative.health()[0].state == rtx::State::stopped,
            "cooperative clock failure did not cleanly stop");
    rtx::Scheduler monitored({{"node", 1, 1ms, 1s, 0ns, {}, [](auto&) {}}});
    rtx::PosixRunner monitor(monitored);
    mode = Mode::background;
    rejected = false;
    try {
      monitor.run();
    } catch (const std::system_error& error) {
      rejected = error.code().value() == EIO;
    }
    mode = Mode::real;
    require(rejected && monitored.health()[0].state == rtx::State::stopped,
            "supervisor clock failure did not propagate and stop");
    // Every background clock read fails: worker/supervisor failures must wake
    // the coordinator and join all launched threads, including workers at
    // launch gates.
    for (unsigned iteration = 0; iteration < 25; ++iteration) {
      rtx::Scheduler scheduler(
          {{"first", 1, 1ms, 1s, 0ns, {}, [](auto&) {}},
           {"second", 2, 1ms, 1s, 0ns, {"first"}, [](auto&) {}}});
      rtx::PosixTaskRunner tasks(scheduler, false);
      mode = Mode::background;
      rejected = false;
      try {
        tasks.run();
      } catch (const std::system_error& error) {
        rejected = error.code().value() == EIO;
      }
      mode = Mode::real;
      require(rejected, "worker clock failure did not propagate");
      for (const auto& health : scheduler.health())
        require(health.state == rtx::State::stopped && health.ticks == 0,
                "worker clock failure leaked execution");
    }
    // Inject supervisor (21) and worker (20) policy setup failures. No launch
    // callback may pass the gate when any participant cannot configure policy.
    for (int priority : {21, 20}) {
      unsigned invoked = 0;
      rtx::Scheduler scheduler(
          {{"first", 10, 1ms, 1s, 0ns, {}, [&](auto&) { ++invoked; }},
           {"second", 20, 1ms, 1s, 0ns, {"first"}, [&](auto&) { ++invoked; }}});
      rtx::PosixTaskRunner tasks(scheduler, true);
      policy_failure_priority = priority;
      rejected = false;
      try {
        tasks.run();
      } catch (const std::system_error& error) {
        rejected = error.code().value() == EPERM;
      }
      policy_failure_priority = 0;
      require(rejected && invoked == 0, "policy setup failure allowed launch");
      for (const auto& health : scheduler.health())
        require(health.state == rtx::State::stopped,
                "policy failure did not stop health");
    }
    std::cout << "POSIX clock conversion, errno propagation, and runner "
                 "cleanup passed\n";
  } catch (const std::exception& error) {
    mode = Mode::real;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
